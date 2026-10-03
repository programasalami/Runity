using System.Text.RegularExpressions;

namespace WaW.AccountService.Accounts;

/// <summary>Registration rules, kept from the reference (AccountServer.md: name 1-10 letters; password longer than 8 characters,
/// not only whitespace). The reference's "must not start with waw-token:" rule is not needed: tokens are never accepted as passwords.</summary>
public static partial class AccountRules
{
    public const int MaxNameLength = 10;
    public const int MinPasswordLength = 9;
    public const int MaxPasswordLength = 128;

    [GeneratedRegex("^[A-Za-z]{1,10}$")]
    private static partial Regex NamePattern();

    public static string? NameError(string? name) =>
        name is not null && NamePattern().IsMatch(name) ? null : "Name must be 1 to 10 letters.";

    public static string? PasswordError(string? password)
    {
        if (string.IsNullOrWhiteSpace(password)) return "Password is required.";
        if (password.Length < MinPasswordLength) return $"Password must be at least {MinPasswordLength} characters.";
        if (password.Length > MaxPasswordLength) return $"Password must be at most {MaxPasswordLength} characters.";
        return null;
    }
}
