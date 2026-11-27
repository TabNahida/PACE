#pragma once

namespace pace
{

/// Address family of a TCP or UDP endpoint or socket. On input, `unspecified` infers the family from the numeric
/// address; endpoint inspection always reports the concrete family of the native socket address.
enum class address_family
{
    unspecified, ///< Infer from the address (input only).
    ipv4,        ///< IPv4.
    ipv6         ///< IPv6, including IPv4-mapped addresses.
};

} // namespace pace
