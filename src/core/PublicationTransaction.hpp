#pragma once
#include "FileUtils.hpp"
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
namespace UltraArch {
// An on-disk intent log. Recovery refuses to overwrite files it cannot identify.
class PublicationTransaction {
  public:
    explicit PublicationTransaction(std::filesystem::path target);
    ~PublicationTransaction();
    const std::filesystem::path &stage() const {
        return stage_;
    }
    void verified(const std::vector<std::filesystem::path> &files);
    void publish(const std::atomic<bool> *cancel = nullptr);
    void rollback();
    std::vector<std::filesystem::path> outputs() const;
    static std::vector<std::filesystem::path> pending(const std::filesystem::path &directory);
    static void recover(const std::filesystem::path &stage);

  private:
    struct Lock;
    std::unique_ptr<Lock> lock_;
    static void recoverImpl(const std::filesystem::path &stage);
    void save(const std::string &phase);
    std::filesystem::path target_, stage_;
    nlohmann::json journal_;
    bool committed_ = false, touched_ = false;
};
} // namespace UltraArch
