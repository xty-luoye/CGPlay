#pragma once
#include <filesystem>
#include <vector>
#include <string>

namespace recorder {
class RuntimeManifest {
public:
    explicit RuntimeManifest(std::filesystem::path root);
    void verify();
    void loadModules();
    void verifyLoadedModules() const;
    const std::string& identity() const { return manifestIdentity; }
    const std::filesystem::path root;
private:
    struct Module { std::string id; std::filesystem::path dll, data; };
    std::vector<Module> modules;
    std::string manifestIdentity;
};
}
