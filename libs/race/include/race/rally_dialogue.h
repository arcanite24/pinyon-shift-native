// The Horizon Rally's spoken lines around a stage (game_rally_race_flow's
// behaviours): the co-driver's pep talk before the countdown
// (CCoDriverPepTalk), the finish call (the stage-finish feedback) and
// Alice's line after the last stage (CPostRaceResults). Each is a
// DialogueScript trigger and, where the behaviour forces one, the variant
// (the trigger's Filename row, 0-based). CRallyStageIntro plays nothing.
#pragma once

#include <string>

namespace race {

struct RallyLine {
  std::string trigger;  // empty: nothing plays
  int variant = -1;     // -1: the player's own choice (round robin)
  bool operator==(const RallyLine&) const = default;
};

// MultiStageSeriesEvents.Id of the final (MS_RALLY_02, Clear Springs).
inline constexpr int kRallyFinalSeries = 7;

// CCoDriverPepTalk before stage `stage` (1-based) of a championship.
// `final_series`: the final; `player_leads`: first in the standings so far.
RallyLine rally_pep_talk(int stage, bool final_series, bool player_leads);

// The co-driver's call at a stage's finish (stage 1-based of `total_stages`;
// `place` the stage place). The heats (a one-stage event) take
// FinishedStage's fourth line; a championship's last stage has none (Alice
// speaks after it).
RallyLine rally_stage_finish_line(int stage, int total_stages, int place, bool heats);

// Alice after a championship's last stage: `won` first overall;
// `first_win_heard` PlayerWonFirstRally has played (the profile flag 4).
RallyLine rally_post_race_line(bool won, bool final_series, bool first_win_heard);

}  // namespace race
