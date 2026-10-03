using System.Security.Cryptography;
using System.Text;

namespace WaW.AccountService.Accounts;

/// <summary>PBKDF2-HMAC-SHA256 with a random 16-byte salt, stored self-describing as
/// <c>pbkdf2-sha256$&lt;iterations&gt;$&lt;salt b64&gt;$&lt;hash b64&gt;</c>.
/// The reference used 100 000 iterations with a text salt in a separate column (AccountServer.md); new hashes use 600 000
/// (current OWASP guidance). The iteration count is stored per hash, so older hashes keep verifying.</summary>
public sealed class PasswordHasher
{
    public const int DefaultIterations = 600_000;
    private const int SaltBytes = 16;
    private const int HashBytes = 32;
    private const string Prefix = "pbkdf2-sha256";

    private readonly int _iterations;

    public PasswordHasher(int iterations = DefaultIterations)
    {
        if (iterations < 1000) throw new ArgumentOutOfRangeException(nameof(iterations));
        _iterations = iterations;
    }

    public string Hash(string password)
    {
        var salt = RandomNumberGenerator.GetBytes(SaltBytes);
        var hash = Rfc2898DeriveBytes.Pbkdf2(Encoding.UTF8.GetBytes(password), salt, _iterations, HashAlgorithmName.SHA256, HashBytes);
        return $"{Prefix}${_iterations}${Convert.ToBase64String(salt)}${Convert.ToBase64String(hash)}";
    }

    public bool Verify(string password, string stored)
    {
        var parts = stored.Split('$');
        if (parts.Length != 4 || parts[0] != Prefix || !int.TryParse(parts[1], out var iterations) || iterations < 1000) return false;
        byte[] salt, expected;
        try
        {
            salt = Convert.FromBase64String(parts[2]);
            expected = Convert.FromBase64String(parts[3]);
        }
        catch (FormatException)
        {
            return false;
        }
        var actual = Rfc2898DeriveBytes.Pbkdf2(Encoding.UTF8.GetBytes(password), salt, iterations, HashAlgorithmName.SHA256, expected.Length);
        return CryptographicOperations.FixedTimeEquals(actual, expected);
    }
}
