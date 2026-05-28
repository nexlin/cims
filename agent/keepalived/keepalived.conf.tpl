! CIMS HA — generic keepalived.conf template (Phase 1.B B 옵션 통합)
!
! 본 파일은 단일 template. `cims-ha config` 가 ha.json 의 `services` 를 반복
! 하면서 PER_SERVICE 블록을 services.* 마다 렌더 → out/keepalived.conf 누적.
!
! 신규 서비스 추가 = ha.json.services 에 항목 1개 (vrid/vip/priority/port 등) 추가.
! 본 파일 수정 불필요.

global_defs {
    enable_script_security
    script_user root
    router_id ${NODE_NAME}
}

{{PER_SERVICE_BEGIN}}
! ── ${SVC_UPPER} — vrrp_script + vrrp_instance ──────────────
vrrp_script check_${SVC} {
    script   "${HA_DIR}/../bin/cims-health ${SVC}"
    interval ${HEALTH_INTERVAL}
    timeout  ${HEALTH_TIMEOUT}
    rise     ${HEALTH_RISE}
    fall     ${HEALTH_FALL}
}

vrrp_instance VI_${SVC_UPPER} {
    state               ${INITIAL_STATE}
    interface           ${INTERFACE}
    virtual_router_id   ${VRID}
    priority            ${PRIORITY}
    advert_int          ${ADVERT_INT}
    ${PREEMPT_LINE}
    unicast_src_ip      ${LOCAL_IP}
    unicast_peer {
        ${PEER_IP}
    }
    authentication {
        auth_type PASS
        auth_pass ${AUTH_PASS}
    }
    virtual_ipaddress {
${VIP_LIST}
    }
${TRACK_INTERFACE_BLOCK}
    track_script {
        check_${SVC}
    }
    notify "${HA_DIR}/../bin/cims-notify ${SVC}"
}

{{PER_SERVICE_END}}
