/*
 * MCData 콘텐츠 저장소 — CSC FD 스토어(csc/src/services/mcdata_fd.py) 호환 기록.
 *
 * MSRP 로 수신한 SDS/FD 를 CSC 콘텐츠 서버와 같은 디렉터리에 기록해, C-plane
 * FILEURL 폴백 수신자가 기존 GET /mcdata/fd/{id} (Bearer) 로 그대로 내려받게 한다.
 *   {Dir}/{YYYY}/{MM}/{DD}/{id}.bin   ← payload 내용 (HTTP 다운로드 대상)
 *   {Dir}/{YYYY}/{MM}/{DD}/{id}.msrp  ← MSRP 원문 본문 (MSRP 재전달용, 인덱스 비노출)
 *   {Dir}/index/{id}.json             ← 메타 (id,name,size,type,group,uploader,ts,path
 *                                        + msrp_content_type 확장키)
 * 메타는 temp+rename 으로 원자 기록 (CSC 가 NAS 를 동시 스캔).
 */

#ifndef _P_FD_STORE_H_
#define _P_FD_STORE_H_

#include <string>

class PFdStore {
public:
    void Init(const std::string& dir) { _dir = dir; }
    bool IsEnabled() const { return !_dir.empty(); }

    /**
     * @brief 수신 완료 메시지 저장.
     * @param binContent  {id}.bin 에 기록할 payload 내용 (폴백 다운로드 대상)
     * @param rawMsrpBody {id}.msrp 에 기록할 MSRP 본문 원문 (재전달용; 비면 생략)
     * @param msrpContentType rawMsrpBody 의 Content-Type (재전달 시 그대로 사용)
     * @param outId [out] 발급된 32-hex file id
     * @return 성공 여부
     */
    bool Store(const std::string& binContent, const std::string& rawMsrpBody,
               const std::string& msrpContentType, const std::string& name,
               const std::string& mime, const std::string& group,
               const std::string& uploader, std::string& outId);

    /** MSRP 재전달용 원문 로드 — index/{id}.json 경유 */
    bool LoadRaw(const std::string& id, std::string& body, std::string& contentType);

    /** 폴백 MESSAGE 조립용 메타 로드 */
    bool LoadMeta(const std::string& id, std::string& name, long long& size, std::string& mime);

private:
    std::string _dir;

    static std::string newFileId();  // uuid4().hex 동형 32-hex
};

#endif
