using System.Windows;

namespace DispatchDesktop.Shell;

public partial class PromptWindow : Window
{
    public string Value => Box.Text;

    public PromptWindow(string title, string label, string initial = "")
    {
        InitializeComponent();
        Title = title; Label.Text = label; Box.Text = initial;
        Loaded += (_, _) => { Box.Focus(); Box.SelectAll(); };
    }

    private void Ok_Click(object sender, RoutedEventArgs e) => DialogResult = true;
}
