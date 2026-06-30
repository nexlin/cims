import netifaces
from typing import Optional


class NetUtil:

    @staticmethod
    def get_mac_address_from_name(nic_name) -> Optional[str]:
        try:
            addresses = netifaces.ifaddresses(nic_name)
            if netifaces.AF_LINK in addresses:
                return addresses[netifaces.AF_LINK][0]['addr']
        except ValueError:
            return None
        return None


if __name__ == '__main__':
    print(NetUtil.get_mac_address_from_name('ens1f1'))