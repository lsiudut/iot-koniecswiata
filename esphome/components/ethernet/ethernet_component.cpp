#include "ethernet_component.h"

#ifdef USE_ETHERNET

namespace esphome::ethernet {

EthernetComponent *global_eth_component;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

EthernetComponent::EthernetComponent() { global_eth_component = this; }

float EthernetComponent::get_setup_priority() const { return setup_priority::WIFI; }

}  // namespace esphome::ethernet

#endif
