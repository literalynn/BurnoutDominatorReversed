#include "MiniTest.h"
#include <cstdlib>
#include <iostream>

void register_code_generator_tests();
void register_r5900_decoder_tests();
void register_elf_analyzer_tests();
void register_pad_input_tests();
void register_ps2_runtime_io_tests();
void register_ps2_runtime_kernel_tests();
void register_ps2_runtime_interrupt_tests();
void register_ps2_memory_tests();
void register_ps2_ipu_tests();
void register_ps2_vu1_tests();
void register_ps2_vu1_upper_engine_tests();
void register_ps2_vu1_program_tests();
void register_ps2_vu_tests();
void register_ps2_gs_tests();
void register_ps2_iop_tests();
void register_ps2_sif_rpc_tests();
void register_ps2_sif_dma_tests();
void register_ps2_recompiler_tests();
void register_ps2_runtime_expansion_tests();
void reset_ps2_test_function_table();

int main()
{
    // Most GS tests read raw VRAM right after a draw: keep the backend
    // immediate unless a test or the caller asks for raster workers.
    if (!std::getenv("BDR_GS_THREADS"))
    {
#ifdef _WIN32
        _putenv_s("BDR_GS_THREADS", "1");
#else
        setenv("BDR_GS_THREADS", "1", 0);
#endif
    }
    // Runtime tests read the guest-side VRAM, which only the CPU rasterizer
    // writes; the paraLLEl-GS backend has its own test.
#ifdef _WIN32
    _putenv_s("PS2X_GS_BACKEND", "cpu");
#else
    setenv("PS2X_GS_BACKEND", "cpu", 1);
#endif
    MiniTest::BeforeEach(reset_ps2_test_function_table);

    register_code_generator_tests();
    register_r5900_decoder_tests();
    register_elf_analyzer_tests();
    register_pad_input_tests();
    register_ps2_runtime_io_tests();
    register_ps2_runtime_kernel_tests();
    register_ps2_runtime_interrupt_tests();
    register_ps2_memory_tests();
    register_ps2_ipu_tests();
    register_ps2_vu1_tests();
    register_ps2_vu1_upper_engine_tests();
    register_ps2_vu1_program_tests();
    register_ps2_vu_tests();
    register_ps2_gs_tests();
    register_ps2_iop_tests();
    register_ps2_sif_rpc_tests();
    register_ps2_sif_dma_tests();
    register_ps2_recompiler_tests();
    register_ps2_runtime_expansion_tests();
    int res = MiniTest::Run();
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(res);
}
