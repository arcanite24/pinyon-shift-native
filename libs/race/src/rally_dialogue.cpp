#include "race/rally_dialogue.h"

namespace race {

RallyLine rally_pep_talk(int stage, bool final_series, bool player_leads) {
  // The leader's line is the first row, the chaser's the second.
  const int lead = player_leads ? 0 : 1;
  switch (stage) {
    case 1: return {};
    case 2: return {"PreRace", -1};
    case 3: return final_series ? RallyLine{"FinalPreRaceSecondStage", lead} : RallyLine{"PreRace", -1};
    case 4: return final_series ? RallyLine{"FinalPreRaceFinalStage", lead} : RallyLine{"PreRaceLastStage", -1};
    default: return {};
  }
}

RallyLine rally_stage_finish_line(int stage, int total_stages, int place, bool heats) {
  if (heats) return {"FinishedStage", 3};
  if (stage < 1 || stage >= total_stages) return {};
  if (place == 1) return {"FinishedStageFirst", -1};
  if (place > 10) return {"FinishedStageLast", -1};
  return {"FinishedStage", -1};
}

RallyLine rally_post_race_line(bool won, bool final_series, bool first_win_heard) {
  if (!won) return {"PostRaceNoWinFeedback", -1};
  if (final_series) return {"PlayerWonRally", 2};
  if (!first_win_heard) return {"PlayerWonFirstRally", -1};
  return {"PlayerWonRally", -1};
}

}  // namespace race
