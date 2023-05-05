/*
 * Copyright (c) 2022-2023, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "3rdparty/INIReader.h"
#include "src/fastertransformer/models/gpt_fp8/GptFP8.h"
#include "src/fastertransformer/utils/mpi_utils.h"
#include "src/fastertransformer/utils/nvtx_utils.h"

#include <cuda_profiler_api.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/time.h>
#include <vector>

using namespace fastertransformer;

template<typename T1, typename T2>
void gpt_example(const INIReader reader, const int output_len);

int main(int argc, char* argv[])
{
    mpi::initialize(&argc, &argv);
    srand(0);
    struct cudaDeviceProp prop;
    check_cuda_error(cudaGetDeviceProperties(&prop, 0));
    printf("Device %s\n", prop.name);

    std::string ini_name;
    if (argc == 2)
        ini_name = std::string(argv[1]);
    else
        ini_name = "../examples/cpp/gpt_fp8/gpt_config.ini";

    INIReader reader = INIReader(ini_name);
    if (reader.ParseError() < 0) {
        std::cout << "[ERROR] Can't load '" << ini_name << "'\n";
        return -1;
    }
    int output_len;
    for (output_len = 128; output_len < 4096; output_len *= 2) {
        gpt_example<__nv_fp8_e4m3, __nv_bfloat16>(reader, output_len);
    }
    mpi::finalize();
    return 0;
}

int read_start_ids(int               batch_size,
                   std::vector<int>* v_start_lengths,
                   std::vector<int>* v_start_ids,
                   int&              max_input_len,
                   const int         end_id,
                   const int         beam_width)
{
    std::vector<std::vector<int>> tmp_start_ids;
    std::vector<int>              tmp_start_lengths;

    std::string   file_name = "../examples/cpp/gpt_fp8/start_ids.csv";
    std::ifstream start_id_file(file_name, std::ios::in);
    if (start_id_file.is_open()) {
        std::string line;
        int         i0 = 0;
        while (std::getline(start_id_file, line)) {
            std::stringstream lineStream(line);
            std::string       vals;
            int               i1 = 0;
            std::vector<int>  tmp_vec;
            while (std::getline(lineStream, vals, ',')) {
                tmp_vec.push_back(std::stoi(vals));
                i1++;
            }
            tmp_start_ids.push_back(tmp_vec);
            tmp_start_lengths.push_back(i1);
            i0++;
        }
    }
    else {
        printf("[WARNING] Cannot open the file '%s'. \n", file_name.c_str());
        max_input_len = 0;
        return 0;
    }

    // max_input_len = tmp_start_lengths.data()[0];
    for (uint i = 1; i < (uint)tmp_start_lengths.size(); i++) {
        max_input_len = max_input_len > tmp_start_lengths.data()[i] ? max_input_len : tmp_start_lengths.data()[i];
    }

    while ((int)tmp_start_lengths.size() < batch_size) {
        std::vector<int> padding_ids;
        for (int i = 0; i < max_input_len; i++) {
            padding_ids.push_back(end_id);
        }
        tmp_start_ids.push_back(padding_ids);
        tmp_start_lengths.push_back(max_input_len);
    }

    // Add padding
    for (int i = 0; i < (int)tmp_start_ids.size(); i++) {
        for (int j = (int)tmp_start_ids[i].size(); j < max_input_len; j++) {
            tmp_start_ids[i].push_back(end_id);
        }
    }

    for (int i = 0; i < (int)tmp_start_ids.size(); i++) {
        for (int b = 0; b < beam_width; b++) {
            for (int j = 0; j < (int)tmp_start_ids[i].size(); j++) {
                v_start_ids->push_back(tmp_start_ids[i][j]);
            }
            v_start_lengths->push_back(tmp_start_lengths[i]);
        }
    }

    return 0;
}

template<typename T1, typename T2>
void gpt_example(const INIReader reader, const int output_len)
{
    const std::string model_name                 = reader.Get("ft_instance_hyperparameter", "model_name");
    const size_t      max_batch_size             = reader.GetInteger("ft_instance_hyperparameter", "max_batch_size");
    const size_t      max_seq_len                = reader.GetInteger("ft_instance_hyperparameter", "max_seq_len");
    const size_t      beam_width                 = reader.GetInteger("ft_instance_hyperparameter", "beam_width");
    const int         top_k                      = reader.GetInteger("ft_instance_hyperparameter", "top_k");
    const float       top_p                      = reader.GetFloat("ft_instance_hyperparameter", "top_p");
    const float       temperature                = reader.GetFloat("ft_instance_hyperparameter", "temperature");
    const float       repetition_penalty         = reader.GetFloat("ft_instance_hyperparameter", "repetition_penalty");
    const std::string model_dir                  = std::string(reader.Get("ft_instance_hyperparameter", "model_dir"));
    const float       len_penalty                = 1.0f;
    const float       beam_search_diversity_rate = 0.0f;
    const unsigned long long int random_seed     = 0;

    const int tensor_para_size   = reader.GetInteger("ft_instance_hyperparameter", "tensor_para_size");
    const int pipeline_para_size = reader.GetInteger("ft_instance_hyperparameter", "pipeline_para_size");

    const size_t head_num       = reader.GetInteger(model_name, "head_num");
    const size_t size_per_head  = reader.GetInteger(model_name, "size_per_head");
    const size_t vocab_size     = reader.GetInteger(model_name, "vocab_size");
    const size_t decoder_layers = reader.GetInteger(model_name, "decoder_layers");
    const size_t hidden_units   = head_num * size_per_head;
    const size_t inter_size     = 4 * hidden_units;

    const size_t request_batch_size = reader.GetInteger("request", "request_batch_size");
    // The length of tokens we hope this model to generate
    const int request_output_len = output_len;

    const int start_id = 50256;
    const int end_id   = 50256;

    FT_CHECK(head_num % tensor_para_size == 0);
    FT_CHECK(decoder_layers % pipeline_para_size == 0);

    // Prepare the parallelism parameters
    int rank       = mpi::getCommWorldRank();
    int world_size = mpi::getCommWorldSize();

    if (rank == 0) {
        FT_LOG_INFO("Total ranks: %d.", world_size);
    }
    int device, device_count;
    check_cuda_error(cudaGetDeviceCount(&device_count));
    check_cuda_error(cudaSetDevice(rank % device_count));
    check_cuda_error(cudaGetDevice(&device));

    struct cudaDeviceProp prop;
    check_cuda_error(cudaGetDeviceProperties(&prop, device));
    FT_LOG_INFO("Device %s", prop.name);
    FT_LOG_INFO("P%d is running with %d GPU.", rank, device);

    FT_CHECK_WITH_INFO(tensor_para_size * pipeline_para_size == world_size,
                       fmtstr("[ERROR] tensor_para_size (%d) * pipeline_para_size (%d) should equal to world_size (%d)",
                              tensor_para_size,
                              pipeline_para_size,
                              world_size));

    const int tensor_para_rank   = rank % tensor_para_size;
    const int pipeline_para_rank = rank / tensor_para_size;
    const int layers_per_group   = decoder_layers / pipeline_para_size;
    if (layers_per_group * pipeline_para_size != (int)decoder_layers) {
        printf("[ERROR] layers_per_group (%d) * pipeline_para_size (%d) should equal to decoder_layers (%ld) \n",
               layers_per_group,
               pipeline_para_size,
               decoder_layers);
        exit(-1);
    }

    // assume gpu_num = k * n,
    // tensor parallelism group size is n
    // pipeline parallelism group size is k
    NcclParam tensor_para;
    NcclParam pipeline_para;
    ftNcclInitialize(tensor_para, pipeline_para, tensor_para_size, pipeline_para_size);

    // Read ids of request from file.
    int              max_input_len = reader.GetInteger("request", "request_input_len", 0);
    std::vector<int> v_start_lengths;
    std::vector<int> v_start_ids;
    read_start_ids(request_batch_size, &v_start_lengths, &v_start_ids, max_input_len, end_id, 1);

    int* d_input_ids;
    int* d_input_lengths;
    if (max_input_len == 0) {
        // unconditional case, no input ids, so do nothing.
        d_input_ids     = nullptr;
        d_input_lengths = nullptr;
    }
    else {
        // conditional case.
        deviceMalloc(&d_input_ids, request_batch_size * max_input_len, false);
        deviceMalloc(&d_input_lengths, request_batch_size, false);
        cudaH2Dcpy(d_input_ids, v_start_ids.data(), request_batch_size * max_input_len);
        cudaH2Dcpy(d_input_lengths, v_start_lengths.data(), request_batch_size);
    }

    const int total_output_len = max_input_len + request_output_len;
    if (total_output_len > (int)max_seq_len) {
        printf("[ERROR] total_output_len (%d) should be <= max_seq_len (%ld). \n", total_output_len, max_seq_len);
        exit(-1);
    }

    std::vector<uint32_t> output_seq_len_array(request_batch_size, total_output_len);

    cudaStream_t     stream;
    cublasHandle_t   cublas_handle;
    cublasLtHandle_t cublaslt_handle;
    cudaStreamCreate(&stream);
    cublasCreate(&cublas_handle);
    cublasLtCreate(&cublaslt_handle);
    cublasSetStream(cublas_handle, stream);
#ifdef SPARSITY_ENABLED
    // cusparseLtHandle_t cusparselt_handle;
    // CHECK_CUSPARSE(cusparseLtInit(&cusparselt_handle));
    // cublasAlgoMap* cublas_algo_map = new cublasAlgoMap(GEMM_CONFIG, SPGEMM_CONFIG);
#else
    cublasAlgoMap* cublas_algo_map = new cublasAlgoMap(GEMM_CONFIG);
#endif

    Allocator<AllocatorType::CUDA> allocator(getDevice());

    std::mutex*        cublas_wrapper_mutex = new std::mutex();
    cublasFP8MMWrapper cublas_wrapper =
        cublasFP8MMWrapper(cublas_handle, cublaslt_handle, stream, cublas_algo_map, cublas_wrapper_mutex, &allocator);

    cublas_wrapper.setGemmConfig(CUDA_R_16BF, CUDA_R_16BF, CUDA_R_16BF, CUDA_R_32F);

    fastertransformer::GptFP8Weight<T1, T2> gpt_weights(hidden_units,
                                                        inter_size,
                                                        vocab_size,
                                                        decoder_layers,
                                                        max_seq_len,
                                                        tensor_para_size,
                                                        tensor_para_rank,
                                                        pipeline_para_size,
                                                        pipeline_para_rank);

    // gpt_weights.loadModel(model_dir);
    gpt_weights.transposeWeight();

#ifdef SPARSITY_ENABLED
    // if (sparse) {
    //     printf("[INFO] Compress weights for sparse inference\n");
    //     gpt_weights.compress_weights(cublas_wrapper);
    // }
#endif

    // Need to remove them in the future.
    GptFP8<T1, T2> gpt = GptFP8<T1, T2>(beam_width,
                                        head_num,
                                        size_per_head,
                                        inter_size,
                                        decoder_layers,
                                        vocab_size,
                                        start_id,
                                        end_id,
                                        tensor_para,
                                        pipeline_para,
                                        stream,
                                        &cublas_wrapper,
                                        &allocator,
                                        false,
                                        &prop,
                                        false);

    int* d_output_ids;
    int* d_sequence_lengths;
    deviceMalloc(&d_output_ids, request_batch_size * beam_width * total_output_len, false);
    deviceMalloc(&d_sequence_lengths, request_batch_size * beam_width, false);

    std::unordered_map<std::string, Tensor> input_tensors = std::unordered_map<std::string, Tensor>{
        {"input_ids",
         Tensor{MEMORY_GPU, TYPE_INT32, std::vector<size_t>{request_batch_size, (size_t)max_input_len}, d_input_ids}},
        {"input_lengths", Tensor{MEMORY_GPU, TYPE_INT32, std::vector<size_t>{request_batch_size}, d_input_lengths}},
        {"output_seq_len",
         Tensor{MEMORY_CPU, TYPE_UINT32, std::vector<size_t>{request_batch_size}, output_seq_len_array.data()}},
        {"temperature", Tensor{MEMORY_CPU, TYPE_FP32, std::vector<size_t>{1}, &temperature}},
        {"len_penalty", Tensor{MEMORY_CPU, TYPE_FP32, std::vector<size_t>{1}, &len_penalty}},
        {"repetition_penalty", Tensor{MEMORY_CPU, TYPE_FP32, std::vector<size_t>{1}, &repetition_penalty}}};
    if (top_k == 0 && top_p == 0.0f) {
        FT_CHECK(beam_width > 1);
        input_tensors.insert({"beam_search_diversity_rate",
                              Tensor{MEMORY_CPU, TYPE_FP32, std::vector<size_t>{1}, &beam_search_diversity_rate}});
    }
    else {
        input_tensors.insert({"random_seed", Tensor{MEMORY_CPU, TYPE_UINT64, std::vector<size_t>{1}, &random_seed}});
        if (top_p != 0.0f) {
            input_tensors.insert({"runtime_top_p", Tensor{MEMORY_CPU, TYPE_FP32, std::vector<size_t>{1}, &top_p}});
        }
        if (top_k != 0) {
            input_tensors.insert({"runtime_top_k", Tensor{MEMORY_CPU, TYPE_INT32, std::vector<size_t>{1}, &top_k}});
        }
    }

    std::unordered_map<std::string, Tensor> output_tensors = std::unordered_map<std::string, Tensor>{
        {"output_ids",
         Tensor{MEMORY_GPU,
                TYPE_INT32,
                std::vector<size_t>{request_batch_size, beam_width, (size_t)total_output_len},
                d_output_ids}},
        {"sequence_length",
         Tensor{MEMORY_GPU, TYPE_INT32, std::vector<size_t>{request_batch_size, beam_width}, d_sequence_lengths}},
        {"output_cum_log_probs",
         Tensor{MEMORY_GPU,
                TYPE_FP32,
                std::vector<size_t>{(size_t)request_output_len, request_batch_size, beam_width},
                nullptr}}};

    print_mem_usage();
    int ite = 5;
    cudaDeviceSynchronize();
    mpi::barrier();

    // warm up
    ite = 1;
    ft_nvtx::setScope("warmup_time");
    PUSH_RANGE("warmup time")
    for (int i = 0; i < ite; ++i) {
        gpt.forward(&output_tensors, &input_tensors, &gpt_weights);
    }
    cudaDeviceSynchronize();
    mpi::barrier();
    POP_RANGE;
    ft_nvtx::resetScope();

    ite = 10;

    struct timeval start, end;
    cudaDeviceSynchronize();
    mpi::barrier();
    cudaProfilerStart();
    gettimeofday(&start, NULL);

    ft_nvtx::setScope("total_time");
    PUSH_RANGE("total time")
    for (int i = 0; i < ite; ++i) {
        gpt.forward(&output_tensors, &input_tensors, &gpt_weights);
    }

    cudaDeviceSynchronize();
    mpi::barrier();
    POP_RANGE;
    ft_nvtx::resetScope();
    gettimeofday(&end, NULL);

    cudaProfilerStop();

    printf("[INFO] request_batch_size %ld beam_width %ld head_num %ld size_per_head %ld total_output_len %d"
           " decoder_layers %ld vocab_size %ld FT-CPP-decoding-beamsearch-time %.2f ms\n",
           request_batch_size,
           beam_width,
           head_num,
           size_per_head,
           total_output_len,
           decoder_layers,
           vocab_size,
           ((end.tv_sec - start.tv_sec) * 1000 + (end.tv_usec - start.tv_usec) * 0.001) / ite);

    // if (rank == 0) {

    //     std::string fName   = "out";
    //     auto        outFile = std::ofstream(fName, std::ios::out);
    //     if (!outFile.is_open()) {
    //         printf("[WARNING] Cannot write results into output file %s \n", fName.c_str());
    //     }
    //     else {
    //         size_t outCount = total_output_len * request_batch_size * beam_width;
    //         int*   hBuf     = new int[outCount];
    //         cudaD2Hcpy(hBuf, d_output_ids, outCount);

    //         {
    //             std::cout << "Writing " << outCount << " elements\n";
    //             int zeroCount = 0;
    //             for (size_t i = 0; i < outCount; i++) {
    //                 if (hBuf[i] == int(0))
    //                     zeroCount++;
    //                 outFile << hBuf[i] << " ";
    //                 if ((i + 1) % (total_output_len) == 0)
    //                     outFile << std::endl;

    //                 if (i < 10)
    //                     printf("%5d ", hBuf[i]);
    //                 if ((i + 1) % (total_output_len) == 0 && i < 10)
    //                     std::cout << std::endl;
    //             }
    //             std::cout << std::endl << "zeroCount = " << zeroCount << std::endl;
    //         }
    //         delete[] hBuf;
    //     }
    // }

#ifdef SPARSITY_ENABLED
    // cusparseLtDestroy(&cusparselt_handle);
#endif
    delete cublas_algo_map;
    delete cublas_wrapper_mutex;
    return;
}
