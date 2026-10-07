#pragma once

#include <ranges>
#include <string_view>
#include <vector>

/// Public API for the template application.
namespace order_book {

/// Lazily split a string view into fields.
///
/// The returned fields alias the storage backing `record`. The caller must keep that
/// buffer alive for as long as the results are used; passing a temporary string dangles.
[[nodiscard]] constexpr auto split_views(std::string_view record, char delimiter = ',')
{
    return record | std::views::split(delimiter) | std::views::transform([](auto subrange) {
               return std::string_view(subrange.begin(), subrange.end());
           });
}

/// Materialize the split fields in a vector.
///
/// The returned field views still alias the storage backing `record`.
[[nodiscard]] std::vector<std::string_view> split_views_vec(std::string_view record,
                                                            char delimiter = ',');

} // namespace order_book
