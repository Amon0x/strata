#include "installed_contract.hpp"

#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

int main() {
    try {
        namespace contract = installed::contract;
        const contract::Installed model{
            .title = "Generated from the installed schema",
            .metadata = {{"source", "installed SDK"}},
            .selection = std::string("alpha"),
            .items = {
                contract::InstalledItemsItem{.key = "alpha", .enabled = true},
            },
        };
        const strata::host::Value snapshot = contract::encode_installed(model);
        const contract::Installed decoded = contract::decode_installed(
            snapshot.require("installed")
        );
        if (decoded.title != model.title || decoded.metadata.at("source") != "installed SDK" ||
            std::get<std::string>(decoded.selection) != "alpha" ||
            decoded.items.size() != 1U || !decoded.items.front().enabled) {
            throw std::runtime_error("generated installed snapshot contract did not round-trip");
        }

        const contract::InstalledRenameAction action = contract::InstalledRenameAction::decode(
            strata::host::ActionEvent{
                std::string(contract::InstalledRenameAction::id),
                strata::host::Value::object({{"name", "renamed"}}),
                "activate",
                std::nullopt,
                {},
            }
        );
        if (action.name != "renamed" || action.priority != 2.0) {
            throw std::runtime_error("generated installed action contract lost its default");
        }
        std::cout << "strata_contract_smoke: installed schema codegen OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strata_contract_smoke: " << error.what() << '\n';
        return 1;
    }
}
