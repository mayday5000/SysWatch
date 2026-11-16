#include "view.hpp"
#include <algorithm>

View::View(int32_t rows) : rows_(rows > 0 ? rows : 20) {
    pending_.reserve(static_cast<size_t>(kPend));
}

void View::append(UiEvent ev) {
    if (paused_) {
        if (pending_.size() < static_cast<size_t>(kPend))
            pending_.push_back(std::move(ev));
        dirty_ = true;
        return;
    }
    push(std::move(ev));
}

void View::on_key(Key key) {
    if (key == Key::Pause) {
        paused_ = !paused_;
        if (!paused_) flush();
        dirty_ = true;
        return;
    }
    if (key == Key::Follow) {
        paused_ = false;
        follow_ = true;
        flush();
        top_ = sel_ = 0;
        dirty_ = true;
        return;
    }
    if (key == Key::Home || key == Key::End) {
        sel_ = (key == Key::Home || n_ == 0) ? 0 : n_ - 1;
        if (!paused_) follow_ = (key == Key::Home);
        clamp();
        dirty_ = true;
        return;
    }
    paused_ = true;
    follow_ = false;
    const int32_t page = rows_ > 1 ? rows_ - 1 : 1;
    switch (key) {
        case Key::Home: sel_ = 0; break;
        case Key::End:  sel_ = n_ ? n_ - 1 : 0; break;
        case Key::Up:   sel_--; break;
        case Key::Down: sel_++; break;
        case Key::PgUp: sel_ -= page; break;
        case Key::PgDn: sel_ += page; break;
        default: break;
    }
    clamp();
    dirty_ = true;
}

void View::set_sort(Sort sort) { sort_ = sort; mark_unsorted(); dirty_ = true; }
void View::set_desc(bool desc) { desc_ = desc; mark_unsorted(); dirty_ = true; }
void View::set_rows(int32_t rows) { rows_ = rows > 0 ? rows : 1; clamp(); dirty_ = true; }

const UiEvent *View::row(int32_t visible) const {
    int32_t i = top_ + visible;
    if (i < 0 || i >= n_) return nullptr;
    if (order_stale_) rebuild();
    return &ring_[static_cast<size_t>(slot_[static_cast<size_t>(order_[static_cast<size_t>(i)])])];
}

void View::push(UiEvent ev) {
    size_t idx;
    if (n_ < kCap) {
        idx = static_cast<size_t>(n_++);
        slot_[idx] = wr_;
    } else {
        std::copy(slot_.begin() + 1, slot_.end(), slot_.begin());
        idx = static_cast<size_t>(kCap - 1);
        slot_[idx] = wr_;
    }
    ring_[static_cast<size_t>(wr_)] = std::move(ev);
    wr_ = (wr_ + 1) % kCap;
    mark_unsorted();
    if (follow_) top_ = sel_ = 0;
    clamp();
    dirty_ = true;
}

void View::flush() {
    if (pending_.empty()) return;
    auto q = std::move(pending_);
    pending_.clear();
    for (auto &ev : q) push(std::move(ev));
}

int32_t View::cmp(int32_t ia, int32_t ib) const {
    const UiEvent &a = ring_[static_cast<size_t>(slot_[static_cast<size_t>(ia)])];
    const UiEvent &b = ring_[static_cast<size_t>(slot_[static_cast<size_t>(ib)])];
    int32_t c = 0;
    switch (sort_) {
        case Sort::Syscall: c = a.sys_name.compare(b.sys_name); break;
        case Sort::Process: c = a.comm.compare(b.comm); break;
        case Sort::SysId:   c = (a.nr > b.nr) - (a.nr < b.nr); break;
        case Sort::Param:   c = a.extra.compare(b.extra); break;
        case Sort::Time:    c = (a.ts_ns > b.ts_ns) - (a.ts_ns < b.ts_ns); break;
        default:            c = (a.pid > b.pid) - (a.pid < b.pid); break;
    }
    if (!c) c = (a.ts_ns > b.ts_ns) - (a.ts_ns < b.ts_ns);
    return desc_ ? -c : c;
}

void View::rebuild() const {
    for (int32_t i = 0; i < n_; i++)
        order_[static_cast<size_t>(i)] = i;
    std::stable_sort(order_.begin(), order_.begin() + n_, [this](int32_t a, int32_t b) {
        return cmp(a, b) < 0;
    });
    order_stale_ = false;
}

void View::clamp() {
    if (n_ <= 0) { top_ = sel_ = 0; return; }
    if (sel_ < 0) sel_ = 0;
    if (sel_ >= n_) sel_ = n_ - 1;
    int32_t max_top = n_ - rows_;
    if (max_top < 0) max_top = 0;
    if (top_ < 0) top_ = 0;
    if (top_ > max_top) top_ = max_top;
    if (sel_ < top_) top_ = sel_;
    if (sel_ >= top_ + rows_) top_ = sel_ - rows_ + 1;
}
