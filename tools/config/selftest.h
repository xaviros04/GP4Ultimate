#pragma once
#include <string>

#include "app.h"

namespace gp4cfg {

class SelfTest {
public:
    SelfTest(std::string ini_path, std::string result_path) : ini_path_(std::move(ini_path)), result_path_(std::move(result_path)) {}
    bool tick(App& app, double now);   // true when finished
    int failures() const { return fail_; }

private:
    void check(bool ok, const std::string& what);
    bool finish();
    std::string ini_path_, result_path_, log_, wheel_;
    int step_ = 0, wait_ = 0, pass_ = 0, fail_ = 0, physics_ = 1;
    bool done_ = false;
};

}  // namespace gp4cfg
