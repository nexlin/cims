/*
 * CIMS extension - Java/C# access to pjsip global settings (pjsip_cfg()) that pjsua2 does not expose.
 * Bound by SWIG through pjsua2.i (org.pjsip.pjsua2.CimsPjCfg). May be called before or after libInit;
 * the value is read at send time. Comments are ASCII only: SWIG copies them into the generated Java
 * and the Android javac runs with a US-ASCII default encoding.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef __PJSUA2_CIMS_CFG_HPP__
#define __PJSUA2_CIMS_CFG_HPP__

#include <pjsua2/config.hpp>

namespace pj
{

/**
 * pjsip global configuration access (CIMS).
 */
class CimsPjCfg
{
public:
    /**
     * Disable/enable the automatic UDP-to-TCP switch of RFC 3261 section 18.1.1 (requests of
     * PJSIP_UDP_SIZE_THRESHOLD bytes or more, 1300 by default, are sent over TCP). true = never switch;
     * large requests stay on UDP and rely on IP fragmentation. Site option for controlled networks only.
     * Same as pjsip_cfg()->endpt.disable_tcp_switch.
     */
    static void setDisableTcpSwitch(bool disable);

    /** Current value. */
    static bool getDisableTcpSwitch();
};

} // namespace pj

#endif  /* __PJSUA2_CIMS_CFG_HPP__ */
