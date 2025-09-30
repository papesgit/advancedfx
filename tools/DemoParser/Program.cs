// Program.cs
using System.Diagnostics;
using System.Text.Json;
using DemoFile;
using DemoFile.Game.Cs;

record MultiKill(
    ulong SteamID, string Player, int Round, int Count,
    int StartTick, int EndTick, string[] Victims
);

// per-section payload (like StringBuilder in the sample)
record SectionData(
    List<Kill> Kills,
    List<int> RoundStarts
);

record Kill(
    int Tick,
    ulong AttackerSid, string AttackerName,
    ulong VictimSid,   string VictimName
);

class Program
{
    static async Task Main(string[] args)
    {
        var path = args.SingleOrDefault() ?? throw new Exception("Expected a single argument: <path to .dem>");
        var sw = Stopwatch.StartNew();

        // Parse in parallel exactly like the sample
        var sections = await DemoFileReader<CsDemoParser>.ReadAllParallelAsync(
            File.ReadAllBytes(path),
            SetupSection,
            default
        );

        // ---------- MERGE PHASE ----------
        // 1) merge & de-dupe kills across sections (boundary overlap)
        var allKills = sections.SelectMany(s => s.Kills)
            .GroupBy(k => (k.Tick, k.AttackerSid, k.VictimSid))
            .Select(g => g.First())
            .OrderBy(k => k.Tick)
            .ToList();

        // 2) merge & sort RoundStart ticks
        var starts = sections.SelectMany(s => s.RoundStarts)
            .Distinct()
            .OrderBy(t => t)
            .ToList();

        // helper: round index via last start ≤ tick (1-based). returns -1 if before first start.
        int RoundOfTick(int tick)
        {
            if (starts.Count == 0) return -1;
            int i = starts.BinarySearch(tick);
            if (i < 0) i = ~i - 1;     // index of last start <= tick
            return i >= 0 ? i + 1 : -1;
        }

        // 3) assign kills to rounds, then aggregate multikills
        var multiKills = allKills
            .Select(k => (Round: RoundOfTick(k.Tick), K: k))
            .Where(x => x.Round > 0) // skip warmup / pregame
            .GroupBy(x => (x.Round, x.K.AttackerSid))
            .Select(g =>
            {
                var kills = g.Select(x => x.K).OrderBy(k => k.Tick).ToList();
                if (kills.Count < 2) return null; // only multi-kills

                // choose the most frequent attacker name we saw in that round
                var name = kills.Select(k => k.AttackerName)
                                .GroupBy(n => n)
                                .OrderByDescending(gg => gg.Count())
                                .First().Key;

                return new MultiKill(
                    g.Key.AttackerSid,
                    name,
                    g.Key.Round,
                    kills.Count,
                    kills.First().Tick,
                    kills.Last().Tick,
                    kills.Select(k => k.VictimName).ToArray()
                );
            })
            .Where(m => m is not null)!
            .OrderBy(m => m!.StartTick)
            .ToList();

        Console.WriteLine(JsonSerializer.Serialize(multiKills, new JsonSerializerOptions { WriteIndented = true }));
        Console.Error.WriteLine($"Finished in {sw.Elapsed.TotalSeconds:N3}s");
    }

    // Mirrors the sample’s SetupSection, but we store Kills + RoundStarts
    private static SectionData SetupSection(CsDemoParser demo)
    {
        var kills = new List<Kill>();
        var starts = new List<int>();

        demo.Source1GameEvents.PlayerDeath += e =>
        {
            if (e.Attacker is null || e.Player is null) return;
            if (ReferenceEquals(e.Attacker, e.Player)) return; // suicides

            kills.Add(new Kill(
                demo.CurrentDemoTick.Value,
                e.Attacker.SteamID, e.Attacker.PlayerName ?? e.Attacker.SteamID.ToString(),
                e.Player.SteamID,   e.Player.PlayerName   ?? "unknown"
            ));
        };

        demo.Source1GameEvents.RoundStart += _ =>
        {
            starts.Add(demo.CurrentDemoTick.Value);
        };

        // We intentionally DO NOT use RoundEnd for round assignment.
        return new SectionData(kills, starts);
    }
}
