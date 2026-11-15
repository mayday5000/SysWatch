#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>

enum class RuleKind : int32_t { 
    Process = 1, 
    SysId = 2, 
    Syscall = 3, 
    Pid = 4 
};

struct FilterRule {
    RuleKind kind = RuleKind::Process;
    int32_t nr = 0;                     // Aligned explicitly with the wire ABI bit-width
    std::string text;
};

class IFilterStore {
public:
    virtual ~IFilterStore() = default;
    
    // Aligned parameter bit-widths with your on-wire sm_event definition
    virtual bool hidden(std::string_view comm, int32_t pid, int32_t nr, std::string_view name) const = 0;
    
    // Uses pass-by-value + move to efficiently handle both lvalues and rvalues
    virtual bool add(FilterRule rule) = 0;
    virtual bool remove_at(int32_t index) = 0;
    virtual const std::vector<FilterRule> &rules() const = 0;
};

class FilterStore final : public IFilterStore {
public:
    explicit FilterStore(std::string path);
    
    bool hidden(std::string_view comm, int32_t pid, int32_t nr, std::string_view name) const override;
    bool add(FilterRule rule) override;
    bool remove_at(int32_t index) override;
    
    const std::vector<FilterRule> &rules() const override { return rules_; }
    bool save() const;
    
    static std::vector<FilterRule> parse(std::string_view xml, int32_t cap = 32);
    static std::vector<FilterRule> seed();
    static bool default_hidden(std::string_view comm, int32_t nr);

private:
    std::string path_;
    std::vector<FilterRule> rules_;
};
