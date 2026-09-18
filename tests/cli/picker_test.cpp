#include "cli/picker.h"
#include "test_support.h"

#include <sstream>
#include <string>
#include <vector>

namespace {

// Picker runs against a real tty stdin in production. The unit tests
// drive it through a redirected istringstream so the test runner
// stays platform-agnostic. With raw mode disabled the picker falls
// back to a "type the index, press enter" prompt — exactly the path
// CI / piped-harness sessions take.

agent::ListPicker make_picker(std::istream& input, std::ostream& output) {
    std::vector<agent::PickerItem> items;
    for (const auto id : {"session-aaa", "session-bbb", "session-ccc"}) {
        agent::PickerItem item;
        item.id = id;
        item.columns.push_back({id, std::string(id), 0, false});
        items.push_back(std::move(item));
    }
    return agent::ListPicker(input, output, std::move(items),
                             "Pick a session");
}

TEST_CASE(picker_selects_by_index_in_fallback_mode) {
    std::istringstream input("2\n");
    std::ostringstream output;
    auto picker = make_picker(input, output);
    const auto outcome = picker.run();
    REQUIRE(outcome.kind == agent::PickerOutcome::Kind::Selected);
    REQUIRE(outcome.index == 2);
    REQUIRE(outcome.id == "session-ccc");
}

TEST_CASE(picker_cancels_on_eof) {
    std::istringstream input;  // immediate EOF
    std::ostringstream output;
    auto picker = make_picker(input, output);
    const auto outcome = picker.run();
    REQUIRE(outcome.kind == agent::PickerOutcome::Kind::Cancelled);
    REQUIRE(outcome.index == -1);
}

TEST_CASE(picker_cancels_on_garbage) {
    std::istringstream input("not a number\n");
    std::ostringstream output;
    auto picker = make_picker(input, output);
    const auto outcome = picker.run();
    REQUIRE(outcome.kind == agent::PickerOutcome::Kind::Cancelled);
}

TEST_CASE(picker_cancels_on_out_of_range) {
    std::istringstream input("99\n");
    std::ostringstream output;
    auto picker = make_picker(input, output);
    const auto outcome = picker.run();
    REQUIRE(outcome.kind == agent::PickerOutcome::Kind::Cancelled);
}

TEST_CASE(picker_handles_empty_item_list) {
    std::istringstream input;
    std::ostringstream output;
    agent::ListPicker picker(input, output, {}, "Empty");
    const auto outcome = picker.run();
    REQUIRE(outcome.kind == agent::PickerOutcome::Kind::Cancelled);
    REQUIRE(outcome.id.empty());
}

}  // namespace