// 관제 외 화면 호스트(§3.4) 코드비하인드 — Screen 에 맞는 화면 VM 을 본문에 붙이고, 주 창 쪽은 별창에 나간 화면이면 자리표시자를 보인다.
using System.Collections.Specialized;
using System.Windows;
using System.Windows.Controls;
using DispatchDesktop.Models;
using DispatchDesktop.ViewModels;

namespace DispatchDesktop.Views;

public partial class ScreenView : UserControl
{
    public static readonly DependencyProperty ScreenProperty =
        DependencyProperty.Register(nameof(Screen), typeof(AppScreen), typeof(ScreenView), new PropertyMetadata(AppScreen.Dispatch, (d, _) => ((ScreenView)d).Update()));
    public static readonly DependencyProperty IsFloatingProperty =
        DependencyProperty.Register(nameof(IsFloating), typeof(bool), typeof(ScreenView), new PropertyMetadata(false, (d, _) => ((ScreenView)d).Update()));
    public static readonly DependencyProperty BodyProperty = DependencyProperty.Register(nameof(Body), typeof(object), typeof(ScreenView));
    public static readonly DependencyProperty TitleProperty = DependencyProperty.Register(nameof(Title), typeof(string), typeof(ScreenView), new PropertyMetadata(""));
    public static readonly DependencyProperty ShowPlaceholderProperty = DependencyProperty.Register(nameof(ShowPlaceholder), typeof(bool), typeof(ScreenView), new PropertyMetadata(false));

    /// <summary>보여 줄 화면 — 주 창은 MainViewModel.Screen 에 바인딩, 별창은 고정.</summary>
    public AppScreen Screen { get => (AppScreen)GetValue(ScreenProperty); set => SetValue(ScreenProperty, value); }
    /// <summary>별창 호스트인가 — [별창으로] 를 숨기고 자리표시자를 쓰지 않는다.</summary>
    public bool IsFloating { get => (bool)GetValue(IsFloatingProperty); set => SetValue(IsFloatingProperty, value); }
    public object? Body { get => GetValue(BodyProperty); private set => SetValue(BodyProperty, value); }
    public string Title { get => (string)GetValue(TitleProperty); private set => SetValue(TitleProperty, value); }
    public bool ShowPlaceholder { get => (bool)GetValue(ShowPlaceholderProperty); private set => SetValue(ShowPlaceholderProperty, value); }

    /// <summary>자리표시자 [별창 앞으로] — 창 관리는 MainWindow.</summary>
    public event EventHandler<AppScreen>? ActivateFloatingRequested;

    private MainViewModel? _vm;

    public ScreenView()
    {
        InitializeComponent();
        DataContextChanged += (_, _) =>
        {
            if (_vm is not null) _vm.PoppedOut.CollectionChanged -= OnPopped;
            _vm = DataContext as MainViewModel;
            if (_vm is not null) _vm.PoppedOut.CollectionChanged += OnPopped;
            Update();
        };
    }

    private void OnPopped(object? sender, NotifyCollectionChangedEventArgs e) => Update();

    private void Update()
    {
        Title = AppScreens.Title(Screen);
        bool popped = !IsFloating && _vm?.PoppedOut.Contains(Screen) == true;
        ShowPlaceholder = popped;
        // 별창에 나간 동안 주 창 쪽 본문은 떼어 둔다(같은 VM 을 두 뷰가 동시에 붙지 않게 — 녹취 재생·비밀번호 상자)
        Body = popped || _vm is null ? null : Screen switch
        {
            AppScreen.History => _vm.HistoryScreen,
            AppScreen.PttGroups => _vm.GroupsScreen,
            AppScreen.Admin => _vm.AdminScreen,
            _ => null,
        };
    }

    private void ActivateFloating_Click(object sender, RoutedEventArgs e) => ActivateFloatingRequested?.Invoke(this, Screen);
}
