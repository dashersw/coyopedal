// Contained ownership prototype: does not change the engine's Node API.
// Reads are pointer + field loads; only write() checks sharing.
#include "ui/node_model.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

using gea::embedded::ui::ComputedStyle;

class SharedStyle {
    struct Record {
        ComputedStyle value{};
        unsigned refs{};
        Record* previous{};
        Record* next{};
    };
    static inline Record* head{};
    static inline unsigned records{};
    static Record& zero() {
        static Record record;
        return record;
    }
    Record* record_ = &zero();

    static Record* create(const ComputedStyle& style) {
        auto* record = new Record{style, 1, nullptr, head};
        if (head)
            head->previous = record;
        head = record;
        ++records;
        return record;
    }
    static void retain(Record* record) {
        if (record != &zero())
            ++record->refs;
    }
    static void release(Record* record) {
        if (record == &zero() || --record->refs != 0)
            return;
        if (record->previous)
            record->previous->next = record->next;
        else
            head = record->next;
        if (record->next)
            record->next->previous = record->previous;
        delete record;
        --records;
    }

  public:
    SharedStyle() = default;
    ~SharedStyle() {
        release(record_);
    }
    SharedStyle(const SharedStyle& other) : record_(other.record_) {
        retain(record_);
    }
    SharedStyle(SharedStyle&& other) noexcept : record_(std::exchange(other.record_, &zero())) {}
    SharedStyle& operator=(const SharedStyle& other) {
        if (this != &other) {
            retain(other.record_);
            release(record_);
            record_ = other.record_;
        }
        return *this;
    }
    SharedStyle& operator=(SharedStyle&& other) noexcept {
        if (this != &other) {
            release(record_);
            record_ = std::exchange(other.record_, &zero());
        }
        return *this;
    }
    const ComputedStyle& read() const {
        return record_->value;
    }
    ComputedStyle& write() {
        if (record_ == &zero() || record_->refs != 1) {
            Record* copy = create(record_->value);
            release(record_);
            record_ = copy;
        }
        return record_->value;
    }
    void intern() {
        // The first prototype excludes rare-style owners, whose handles have
        // separate lifecycle semantics. Padding may prevent a merge, never
        // merge unequal values. Production should compare semantic fields.
        if (record_ == &zero() || record_->value.rare_style >= 0)
            return;
        for (Record* candidate = head; candidate; candidate = candidate->next) {
            if (candidate == record_ ||
                std::memcmp(&candidate->value, &record_->value, sizeof(ComputedStyle)))
                continue;
            retain(candidate);
            release(record_);
            record_ = candidate;
            return;
        }
    }
    static unsigned count() {
        return records;
    }
    static std::size_t bytes() {
        return records * sizeof(Record);
    }
};

// Target assembly probes: a read, a grouped read, and a shared-write boundary.
extern "C" __attribute__((noinline)) int shared_style_width(const SharedStyle& style) {
    return style.read().width;
}
extern "C" __attribute__((noinline)) int inline_style_width(const ComputedStyle& style) {
    return style.width;
}
extern "C" __attribute__((noinline)) int shared_style_group(const SharedStyle& style) {
    const auto& s = style.read();
    return s.width + s.height + s.font_size + s.font_weight + s.padding[0] + s.padding[1] +
           s.margin[0] + s.margin[1];
}

int main() {
    assert(SharedStyle::count() == 0);
    {
        std::vector<SharedStyle> nodes(160);
        assert(SharedStyle::count() == 0);
        for (unsigned i = 0; i < nodes.size(); ++i) {
            auto& s = nodes[i].write();
#if GEA_CSS_RARE_STYLE && !GEA_EMBEDDED_RARE_STYLE_INLINE
            s.rare_style = -1;
#endif
            s.width = static_cast<int>(i % 10 + 1);
            s.height = 16;
            s.font_weight = 400;
            nodes[i].intern();
        }
        assert(SharedStyle::count() == 10);
        std::printf("shared-static nodes=160 unique=%u handles=%zu records=%zu inline=%zu\n",
                    SharedStyle::count(), sizeof(SharedStyle) * nodes.size(), SharedStyle::bytes(),
                    sizeof(ComputedStyle) * nodes.size());
        auto copy = nodes[0];
        assert(&copy.read() == &nodes[0].read());
        copy.write().width = 100;
        assert(copy.read().width == 100 && nodes[0].read().width == 1);
        auto moved = std::move(copy);
        assert(moved.read().width == 100 && copy.read().width == 0);
        moved = moved;
        assert(moved.read().width == 100);
        moved = nodes[0];
        assert(&moved.read() == &nodes[0].read());
        for (unsigned frame = 0; frame < 2000; ++frame) {
            for (unsigned i = 0; i < 64; ++i) {
                auto& s = nodes[i].write();
                s.width = static_cast<int>((frame + i) % 600);
                assert(s.width == static_cast<int>((frame + i) % 600));
            }
            assert(nodes[70].read().width == 1);
        }
        assert(SharedStyle::count() == 74);
        std::printf("dynamic nodes=160 moving=64 unique=%u handles=%zu records=%zu\n",
                    SharedStyle::count(), sizeof(SharedStyle) * nodes.size(), SharedStyle::bytes());
    }
    assert(SharedStyle::count() == 0);
    std::puts("PASS copy, detach, move, reuse, 128000 dynamic writes, complete reclamation");
}
