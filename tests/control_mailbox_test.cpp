#include "audio/control_mailbox.hpp"
#include <array>
#include <cassert>
#include <thread>

int main() {
    using Value = std::array<unsigned, 32>;
    coyopedal::pedal::ControlMailbox<Value> mailbox;
    assert(mailbox.consume() == nullptr);
    Value value{};
    value.fill(1U);
    mailbox.publish(value);
    const Value* held = mailbox.consume();
    assert(held && (*held)[0] == 1U);
    for (unsigned i = 2U; i <= 100U; ++i) {
        value.fill(i);
        mailbox.publish(value);
        for (unsigned item : *held)
            assert(item == 1U);
    }
    held = mailbox.consume();
    assert(held && (*held)[0] == 100U);
    assert(mailbox.consume() == nullptr);

    constexpr unsigned last = 200000U;
    std::thread writer([&] {
        Value next{};
        for (unsigned i = 101U; i <= last; ++i) {
            next.fill(i);
            mailbox.publish(next);
        }
    });
    unsigned previous = 100U;
    while (previous != last) {
        if (const Value* current = mailbox.consume()) {
            assert((*current)[0] > previous);
            previous = (*current)[0];
            for (unsigned item : *current)
                assert(item == previous);
        } else {
            std::this_thread::yield();
        }
    }
    writer.join();
}
