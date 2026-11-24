// CBZ / CBNZ end-to-end execution tests.
//
// The decoders used to drop the branch target entirely (the old CondBranch had
// no operands), so nothing that relied on CBZ/CBNZ could work. These tests run
// generated programs that exercise the branch both ways, plus a countdown loop
// that only terminates if CBNZ targets the right guest word.
//
// Skipped when g++ is not on PATH; compiled with UBSan first, then plainly.
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "switchrecomp/codegen/codegen.h"
#include "switchrecomp/common/span.h"
#include "switchrecomp/translator/translator.h"

#ifndef SR_INCLUDE_DIR
#error "SR_INCLUDE_DIR must be defined via CMake"
#endif

#ifdef _WIN32
#define SR_POPEN _popen
#define SR_PCLOSE _pclose
#else
#define SR_POPEN popen
#define SR_PCLOSE pclose
#endif

namespace {

std::string ProjectRoot() {
    std::string inc = SR_INCLUDE_DIR;
    const std::string sep = "/include";
    const std::string sep_win = "\\include";
    if (inc.size() >= sep.size() &&
        inc.compare(inc.size() - sep.size(), sep.size(), sep) == 0) {
        return inc.substr(0, inc.size() - sep.size());
    }
    if (inc.size() >= sep_win.size() &&
        inc.compare(inc.size() - sep_win.size(), sep_win.size(), sep_win) == 0) {
        return inc.substr(0, inc.size() - sep_win.size());
    }
    return inc;
}

std::string RunCapture(const std::string& cmd) {
    std::string output;
    FILE* pipe = SR_POPEN(cmd.c_str(), "r");
    if (!pipe) return "";
    char buf[512];
    while (std::fgets(buf, sizeof(buf), pipe)) {
        output += buf;
    }
    SR_PCLOSE(pipe);
    return output;
}

bool HasGxx() {
#ifdef _WIN32
    const char* null_dev = "nul";
#else
    const char* null_dev = "/dev/null";
#endif
    std::string cmd = std::string("g++ --version > ") + null_dev + " 2>&1";
    return std::system(cmd.c_str()) == 0;
}

std::string BuildRun(const uint8_t* code, size_t size,
                     const std::string& stem) {
    const std::string root = ProjectRoot();
    const std::string include_dir = root + "/include";
    const std::string runtime_dir = root + "/src/runtime";
    const std::string platform_dir = root + "/src/platform";
    const std::string out_c = stem + ".c";
    const std::string out_exe = stem + ".exe";

    auto result = sr::translator::Translate(
        sr::common::span<const uint8_t>(code, size), {});
    if (!result) return "";

    auto c_src = sr::codegen::GenerateC(*result.value(), {});
    if (!c_src) return "";

    std::string full = c_src.value();
    full += R"(
#include <stdio.h>
#include <string.h>
int main() {
    GuestContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    fn_0(&ctx);
    printf("result = %lld\n", (long long)(int64_t)ctx.x[0]);
    return 0;
}
)";

    {
        std::ofstream f(out_c, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) return "";
        f << full;
    }

    const std::string sources =
        "\"" + out_c + "\" \"" + runtime_dir + "/runtime.cpp\" \"" +
        runtime_dir + "/memory_manager.cpp\" \"" +
        runtime_dir + "/svc_table.cpp\" \"" +
        runtime_dir + "/svc_thread.cpp\" \"" +
        runtime_dir + "/hle_service.cpp\" \"" +
        platform_dir + "/memory.cpp\" \"" +
        platform_dir + "/thread.cpp\"";

    auto compile = [&](const std::string& extra) {
        std::string c =
            "g++ -std=c++17 " + extra + " -I \"" + include_dir + "\" -I \"" +
            runtime_dir + "\" " + sources + " -o \"" + out_exe + "\" 2>&1";
        return std::system(c.c_str());
    };
    if (compile("-fsanitize=undefined -fno-sanitize-recover=all") != 0 &&
        compile("") != 0) {
        return "";
    }

#ifdef _WIN32
    return RunCapture("\"" + out_exe + "\"");
#else
    return RunCapture("\"./" + out_exe + "\"");
#endif
}

bool ExpectResult(const std::string& output, long long expected) {
    const std::string needle = "result = " + std::to_string(expected);
    return output.find(needle) != std::string::npos;
}

}  // namespace

TEST(E2ECompareBranch, CbnzCountdownLoop) {
    if (!HasGxx()) {
        GTEST_SKIP() << "g++ not available on PATH; skipping e2e compile test";
    }

    // x0 = 0; x1 = 3; loop { x0++; x1--; cbnz x1, loop }; ret -> x0 = 3.
    const uint8_t code[] = {
        0x00, 0x00, 0x80, 0xD2,  // movz x0,#0   0xD2800000
        0x61, 0x00, 0x80, 0xD2,  // movz x1,#3   0xD2800061
        0x00, 0x04, 0x00, 0x91,  // add x0,x0,#1 0x91000400
        0x21, 0x04, 0x00, 0xD1,  // sub x1,x1,#1 0xD1000421
        0xC1, 0xFF, 0xFF, 0x35,  // cbnz x1,-8   0x35FFFFC1
        0xC0, 0x03, 0x5F, 0xD6,  // ret          0xD65F03C0
    };

    const std::string output = BuildRun(code, sizeof(code), "e2e_cbnz_loop");
    ASSERT_FALSE(output.empty()) << "build/run pipeline produced no output";
    EXPECT_TRUE(ExpectResult(output, 3)) << "expected 'result = 3', got:\n"
                                        << output;
}

TEST(E2ECompareBranch, CbzTaken) {
    if (!HasGxx()) {
        GTEST_SKIP() << "g++ not available on PATH; skipping e2e compile test";
    }

    // x1 = 0 -> cbz taken -> word 4 -> x0 = 7.
    const uint8_t code[] = {
        0x01, 0x00, 0x80, 0xD2,  // movz x1,#0    0xD2800001
        0x61, 0x00, 0x00, 0x34,  // cbz x1,+12    0x34000061
        0x00, 0x01, 0x80, 0xD2,  // movz x0,#8    0xD2800100
        0x02, 0x00, 0x00, 0x14,  // b +8          0x14000002
        0xE0, 0x00, 0x80, 0xD2,  // movz x0,#7    0xD28000E0
        0xC0, 0x03, 0x5F, 0xD6,  // ret           0xD65F03C0
    };

    const std::string output = BuildRun(code, sizeof(code), "e2e_cbz_taken");
    ASSERT_FALSE(output.empty()) << "build/run pipeline produced no output";
    EXPECT_TRUE(ExpectResult(output, 7)) << "expected 'result = 7', got:\n"
                                        << output;
}

TEST(E2ECompareBranch, CbzNotTaken) {
    if (!HasGxx()) {
        GTEST_SKIP() << "g++ not available on PATH; skipping e2e compile test";
    }

    // x1 = 1 -> cbz not taken -> word 2 -> x0 = 8.
    const uint8_t code[] = {
        0x21, 0x00, 0x80, 0xD2,  // movz x1,#1    0xD2800021
        0x61, 0x00, 0x00, 0x34,  // cbz x1,+12    0x34000061
        0x00, 0x01, 0x80, 0xD2,  // movz x0,#8    0xD2800100
        0x02, 0x00, 0x00, 0x14,  // b +8          0x14000002
        0xE0, 0x00, 0x80, 0xD2,  // movz x0,#7    0xD28000E0
        0xC0, 0x03, 0x5F, 0xD6,  // ret           0xD65F03C0
    };

    const std::string output = BuildRun(code, sizeof(code), "e2e_cbz_nottaken");
    ASSERT_FALSE(output.empty()) << "build/run pipeline produced no output";
    EXPECT_TRUE(ExpectResult(output, 8)) << "expected 'result = 8', got:\n"
                                        << output;
}
