#pragma once

namespace pace {

// Endpoint input may infer the family from its numeric address. Inspection
// always reports the concrete family of the native socket address.
enum class address_family { unspecified, ipv4, ipv6 };

} // namespace pace
