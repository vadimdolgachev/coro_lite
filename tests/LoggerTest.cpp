#include "Logger.h"

#include "TestCheck.h"
#include <sstream>
#include <string_view>

int main(const int argc, char **argv) {
    check(argc == 2);
    const bool verbose = std::string_view(argv[1]) == "1";
    std::ostringstream output, errors;
    auto *const oldOutput = std::cout.rdbuf(output.rdbuf());
    auto *const oldErrors = std::cerr.rdbuf(errors.rdbuf());
    CoroLite::log_d("debug-marker\n");
    CoroLite::log_v("verbose-marker\n");
    CoroLite::log_e("error-marker\n");
    std::cout.rdbuf(oldOutput);
    std::cerr.rdbuf(oldErrors);
    check(output.str().contains("debug-marker"));
    check(output.str().contains("verbose-marker") == verbose);
    check(!output.str().contains("error-marker"));
    check(errors.str().contains("error-marker"));
    std::string otherId;
    std::thread worker([&] { otherId = threadId(); });
    worker.join();
    check(!threadId().empty() && threadId() != otherId);
}
