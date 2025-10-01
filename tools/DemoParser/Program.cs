// Program.cs
using System.Diagnostics;
using System.Text.Json;
using DemoFile;
using DemoFile.Game.Cs;
using System.Linq;

record RoundKills(
    int Round,
    int StartTick,
    List<KillEvent> Kills
);

record KillEvent(
    int Tick,
    ulong AttackerSid, string AttackerName, string AttackerTeam,
    ulong VictimSid,   string VictimName, string VictimTeam,
    string Weapon,
    bool Headshot,
    bool Noscope,
    bool Attackerinair,
    bool Attackerblind,
    int  Penetrated,
    bool Thrusmoke
);

// Data we collect per parallel section
record SectionData(
    List<KillEvent> Kills,
    List<int> RoundStarts
);

class Program
{
    static async Task Main(string[] args)
    {
        var path = args.SingleOrDefault() ?? throw new Exception("Expected a single argument: <path to .dem>");
        var sw = Stopwatch.StartNew();

        var sections = await DemoFileReader<CsDemoParser>.ReadAllParallelAsync(
            File.ReadAllBytes(path),
            SetupSection,
            default
        );

        // ---- MERGE PHASE ----

        // Merge & de-dupe kills across sections (boundary overlap)
        var allKills = sections
            .SelectMany(s => s.Kills)
            .GroupBy(k => (k.Tick, k.AttackerSid, k.VictimSid)) // sufficient to de-dupe
            .Select(g => g.First())
            .OrderBy(k => k.Tick)
            .ToList();

        // Merge & sort RoundStart ticks
        var starts = sections.SelectMany(s => s.RoundStarts)
            .Distinct()
            .OrderBy(t => t)
            .ToList();

        // Prepare an output round bucket for each start
        var rounds = starts.Select((s, i) => new RoundKills(i + 1, s, new List<KillEvent>())).ToList();

        // Helper: find last start <= tick (binary search). Returns index, or -1 if before first start.
        int IndexOfRound(int tick)
        {
            if (rounds.Count == 0) return -1;
            var startTicks = starts; // alias
            int i = startTicks.BinarySearch(tick);
            if (i < 0) i = ~i - 1;
            return i;
        }

        // Assign each kill to its round
        foreach (var k in allKills)
        {
            int idx = IndexOfRound(k.Tick);
            if (idx >= 0 && idx < rounds.Count)
                rounds[idx].Kills.Add(k);
        }

        // Optional: drop empty rounds (if any)
        var output = rounds.Where(r => r.Kills.Count > 0).ToList();

        Console.WriteLine(JsonSerializer.Serialize(output, new JsonSerializerOptions { WriteIndented = true }));
        Console.Error.WriteLine($"Finished in {sw.Elapsed.TotalSeconds:N3}s");
    }

    // Mirrors the repo's MultiThreaded SetupSection, but records richer kill info + RoundStarts
    private static SectionData SetupSection(CsDemoParser demo)
    {
        var kills = new List<KillEvent>();
        var starts = new List<int>();

        demo.Source1GameEvents.PlayerDeath += e =>
        {
            if (e.Attacker is null || e.Player is null) return;
            if (ReferenceEquals(e.Attacker, e.Player)) return; // ignore suicides

            var tick = demo.CurrentDemoTick.Value;

            // NB: Property names use what you listed (Noscope, Attackerinair, Attackerblind, Penetrated).
            var weapon   = e.Weapon ?? "";
            var headshot = e.Headshot;
            var noscope  = e.Noscope;
            var inAir    = e.Attackerinair;
            var blind    = e.Attackerblind;
            var pen      = e.Penetrated;
            var smoke    = e.Thrusmoke;

            kills.Add(new KillEvent(
                Tick: tick,
                AttackerSid: e.Attacker.SteamID,
                AttackerName: e.Attacker.PlayerName ?? e.Attacker.SteamID.ToString(),
                AttackerTeam: e.Attacker.Team.Teamname,
                VictimSid: e.Player.SteamID,
                VictimName: e.Player.PlayerName ?? "unknown",
                VictimTeam: e.Player.Team.Teamname,
                Weapon: weapon,
                Headshot: headshot,
                Noscope: noscope,
                Attackerinair: inAir,
                Attackerblind: blind,
                Penetrated: pen,
                Thrusmoke: smoke
            ));
        };

        demo.Source1GameEvents.RoundStart += _ =>
        {
            starts.Add(demo.CurrentDemoTick.Value);
        };

        demo.Source1GameEvents.RoundAnnounceMatchStart += _ =>
        {
            starts.Add(demo.CurrentDemoTick.Value);
        };

        return new SectionData(kills, starts);
    }
}
