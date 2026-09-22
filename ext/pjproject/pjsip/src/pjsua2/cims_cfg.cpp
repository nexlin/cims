/*
 * CIMS extension - pjsua2 CimsPjCfg implementation. See cims_cfg.hpp.
 */
#include <pjsua2/cims_cfg.hpp>
#include <pjsip.h>

using namespace pj;

void CimsPjCfg::setDisableTcpSwitch(bool disable)
{
    pjsip_cfg()->endpt.disable_tcp_switch = disable ? PJ_TRUE : PJ_FALSE;
}

bool CimsPjCfg::getDisableTcpSwitch()
{
    return pjsip_cfg()->endpt.disable_tcp_switch ? true : false;
}
