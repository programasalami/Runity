using System.Text.RegularExpressions;
using WaW.AccountService.Accounts;
using WaW.AccountService.Database;
using WaW.AccountService.Sessions;

namespace WaW.AccountService.Tests;

public class RulesTests
{
    [Theory]
    [InlineData("Bob", true)]
    [InlineData("abcdefghij", true)]
    [InlineData("abcdefghijk", false)]
    [InlineData("", false)]
    [InlineData("Bob1", false)]
    [InlineData("Bo b", false)]
    [InlineData("Bób", false)]
    public void NamesAreOneToTenAsciiLetters(string name, bool valid) => Assert.Equal(valid, AccountRules.NameError(name) is null);

    [Theory]
    [InlineData("12345678", false)]
    [InlineData("123456789", true)]
    [InlineData("         ", false)]
    [InlineData(null, false)]
    public void PasswordsAreLongerThanEightCharacters(string? password, bool valid) =>
        Assert.Equal(valid, AccountRules.PasswordError(password) is null);

    [Fact]
    public void PasswordHashesVerifyAndAreSalted()
    {
        var hasher = new PasswordHasher(1000);
        var a = hasher.Hash("correct horse");
        var b = hasher.Hash("correct horse");
        Assert.NotEqual(a, b);
        Assert.StartsWith("pbkdf2-sha256$1000$", a);
        Assert.True(hasher.Verify("correct horse", a));
        Assert.False(hasher.Verify("correct horsf", a));
    }

    [Fact]
    public void AHashKeepsVerifyingWhenTheDefaultIterationCountChanges()
    {
        var stored = new PasswordHasher(1000).Hash("password123");
        Assert.True(new PasswordHasher(5000).Verify("password123", stored));
    }

    [Theory]
    [InlineData("")]
    [InlineData("pbkdf2-sha256$abc$AAAA$AAAA")]
    [InlineData("pbkdf2-sha256$1000$not-base64!$AAAA")]
    [InlineData("md5$1000$AAAA$AAAA")]
    [InlineData("pbkdf2-sha256$10$AAAA$AAAA")]
    public void MalformedStoredHashesNeverVerify(string stored) => Assert.False(new PasswordHasher(1000).Verify("x", stored));

    [Fact]
    public void TokenHashIsLowerCaseHexSha256()
    {
        // NIST test vector: SHA-256("abc"). The C++ GameServer hashes join tickets the same way.
        Assert.Equal("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", Tokens.Hash("abc"));
    }

    [Fact]
    public void TokensAreUrlSafeAndUnique()
    {
        var a = Tokens.NewToken();
        Assert.Matches("^[A-Za-z0-9_-]{43}$", a);
        Assert.NotEqual(a, Tokens.NewToken());
    }

    [Fact]
    public void ConnInfoConvertsLibpqKeys()
    {
        var s = ConnInfo.ToNpgsql("host=db port=5433 dbname=waw user=u password=p");
        Assert.Contains("Host=db", s);
        Assert.Contains("Port=5433", s);
        Assert.Contains("Database=waw", s);
        Assert.Contains("Username=u", s);
        Assert.Throws<FormatException>(() => ConnInfo.ToNpgsql("hostdb"));
        Assert.Throws<FormatException>(() => ConnInfo.ToNpgsql("weird=1"));
    }

    private static string RepoRoot()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir is not null && !Directory.Exists(Path.Combine(dir.FullName, "Protocol", "schema"))) dir = dir.Parent;
        return dir?.FullName ?? throw new InvalidOperationException("repository root not found");
    }

    [Fact]
    public void MigrationsAreNumberedAndOrdered()
    {
        var migrations = MigrationRunner.Discover(Path.Combine(RepoRoot(), "Database", "migrations"));
        Assert.NotEmpty(migrations);
        Assert.Equal(1, migrations[0].Version);
        Assert.Equal(migrations.Select(m => m.Version).OrderBy(v => v), migrations.Select(m => m.Version));
    }

    [Fact]
    public void ConfiguredProtocolVersionMatchesTheSchema()
    {
        var toml = File.ReadAllText(Path.Combine(RepoRoot(), "Protocol", "schema", "protocol.toml"));
        var schemaVersion = int.Parse(Regex.Match(toml, @"protocol_version\s*=\s*(\d+)").Groups[1].Value);
        Assert.Equal(schemaVersion, new ServiceOptions().ProtocolVersion);
        var appsettings = File.ReadAllText(Path.Combine(RepoRoot(), "AccountService", "src", "WaW.AccountService", "appsettings.json"));
        Assert.Matches($"\"ProtocolVersion\":\\s*{schemaVersion}\\b", appsettings);
    }
}
