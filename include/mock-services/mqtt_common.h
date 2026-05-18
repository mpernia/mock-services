#pragma once

#include <cstdint>

namespace mock_services {
namespace mqtt {

namespace packet {
constexpr uint8_t connect    = 0x10;
constexpr uint8_t connack    = 0x20;
constexpr uint8_t publish    = 0x30;
constexpr uint8_t subscribe  = 0x82;
constexpr uint8_t suback     = 0x90;
constexpr uint8_t pingreq    = 0xC0;
constexpr uint8_t pingresp   = 0xD0;
constexpr uint8_t disconnect = 0xE0;
}

namespace qos {
constexpr int at_most_once   = 0;
constexpr int at_least_once  = 1;
constexpr int exactly_once   = 2;
}

namespace connect_return {
constexpr int accepted               = 0;
constexpr int unacceptable_protocol   = 1;
constexpr int identifier_rejected     = 2;
constexpr int server_unavailable      = 3;
constexpr int bad_user_or_password    = 4;
constexpr int not_authorized          = 5;
}

static_assert(packet::connect    == 0x10, "bad packet constant");
static_assert(packet::connack    == 0x20, "bad packet constant");
static_assert(packet::publish    == 0x30, "bad packet constant");
static_assert(packet::subscribe  == 0x82, "bad packet constant");
static_assert(packet::suback     == 0x90, "bad packet constant");
static_assert(packet::pingreq    == 0xC0, "bad packet constant");
static_assert(packet::pingresp   == 0xD0, "bad packet constant");
static_assert(packet::disconnect == 0xE0, "bad packet constant");

static_assert(qos::at_most_once  == 0, "bad qos constant");
static_assert(qos::at_least_once == 1, "bad qos constant");
static_assert(qos::exactly_once  == 2, "bad qos constant");

static_assert(connect_return::accepted             == 0, "bad return code");
static_assert(connect_return::unacceptable_protocol == 1, "bad return code");
static_assert(connect_return::identifier_rejected   == 2, "bad return code");
static_assert(connect_return::server_unavailable    == 3, "bad return code");
static_assert(connect_return::bad_user_or_password  == 4, "bad return code");
static_assert(connect_return::not_authorized        == 5, "bad return code");

}
}
