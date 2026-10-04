#include <optional.h>
#include <sapling/zip32.h>
#include <optional>
#include <variant>
#include <type_traits>
static_assert(std::is_same_v<Optional<int>,std::optional<int>>);
static_assert(std::is_same_v<libzcash::PaymentAddress,std::variant<libzcash::InvalidEncoding,libzcash::SaplingPaymentAddress>>);
static_assert(std::is_same_v<libzcash::ViewingKey,std::variant<libzcash::InvalidEncoding,libzcash::SaplingExtendedFullViewingKey>>);
static_assert(std::is_same_v<libzcash::SpendingKey,std::variant<libzcash::InvalidEncoding,libzcash::SaplingExtendedSpendingKey>>);
