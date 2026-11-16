#pragma once
#include "types.hpp"
#include <array>
#include <vector>

class View final : public IView {
    static constexpr int32_t kCap = 512;
    static constexpr int32_t kPend = 64;
public:
    explicit View(int32_t rows);
    void append(UiEvent ev) override;
    void on_key(Key key) override;
    void set_sort(Sort sort) override;
    Sort sort() const override { return sort_; }
    void set_desc(bool desc) override;
    bool desc() const override { return desc_; }
    void set_rows(int32_t rows) override;
    const UiEvent *row(int32_t visible) const override;
    int32_t count() const override { return n_; }
    int32_t top() const override { return top_; }
    int32_t sel() const override { return sel_; }
    bool paused() const override { return paused_; }
    bool dirty() const override { return dirty_; }
    void mark_drawn() override { dirty_ = false; }
    int32_t pending() const { return static_cast<int32_t>(pending_.size()); }

private:
    void push(UiEvent ev);
    void flush();
    int32_t cmp(int32_t ia, int32_t ib) const;
    // Sorting is deferred until a row is read (once per frame), instead of
    // re-sorting all rows for every appended event.
    void rebuild() const;
    void mark_unsorted() { order_stale_ = true; }
    void clamp();

    std::array<UiEvent, 512> ring_{};
    std::array<int32_t, 512> slot_{};
    mutable std::array<int32_t, 512> order_{};
    mutable bool order_stale_ = true;
    std::vector<UiEvent> pending_;
    int32_t n_ = 0;
    int32_t wr_ = 0;
    int32_t top_ = 0;
    int32_t sel_ = 0;
    int32_t rows_;
    bool follow_ = true;
    bool paused_ = false;
    bool desc_ = true;
    bool dirty_ = true;
    Sort sort_ = Sort::Time;
};
