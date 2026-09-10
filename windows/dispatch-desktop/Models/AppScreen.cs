// 최상위 메뉴 = 주 화면(§3.4). 관제 캔버스(도킹 호스트)는 항상 마운트돼 있고, 나머지 셋은 그 위에 겹치는 화면 레이어다.
namespace DispatchDesktop.Models;

public enum AppScreen
{
    /// <summary>[관제] — §3.1 도킹 캔버스.</summary>
    Dispatch,
    /// <summary>[이력] — 끝난 세션의 날짜 창 조회 + 녹취 재생(§4.6).</summary>
    History,
    /// <summary>[PTT 그룹] — 관리 범위 안 그룹 목록·생성/편집/삭제(§4.7).</summary>
    PttGroups,
    /// <summary>[관리] — 조직·구성원·번호(§4.5). 관리 범위(dispatch.directoryAdmin)가 있어야 활성.</summary>
    Admin,
}

public static class AppScreens
{
    public static string Title(AppScreen s) => s switch
    {
        AppScreen.History => "이력",
        AppScreen.PttGroups => "PTT 그룹",
        AppScreen.Admin => "조직 · 구성원 · 번호",
        _ => "관제",
    };

    /// <summary>화면 전환 핫키(§8) — 고정 F1~F4. 설정 핫키가 같은 키를 쓰면 설정 쪽이 우선한다.</summary>
    public static AppScreen? OfFunctionKey(System.Windows.Input.Key k) => k switch
    {
        System.Windows.Input.Key.F1 => AppScreen.Dispatch,
        System.Windows.Input.Key.F2 => AppScreen.History,
        System.Windows.Input.Key.F3 => AppScreen.PttGroups,
        System.Windows.Input.Key.F4 => AppScreen.Admin,
        _ => null,
    };
}
