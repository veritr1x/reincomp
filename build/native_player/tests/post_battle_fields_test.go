package questflow

import (
	"testing"

	"lunar-tear/server/internal/masterdata"
	"lunar-tear/server/internal/model"
)

func TestFinishBeforePostBattleFieldScenes(t *testing.T) {
	const first, result, bridge, ending int32 = 100, 101, 102, 103
	h := testHandler()
	h.QuestById[testQuest] = masterdata.EntityMQuest{QuestId: testQuest, IsCountedAsQuest: true}
	h.SceneIdsByQuestId = map[int32][]int32{testQuest: {first, result, bridge, ending}}
	half := int32(model.QuestResultTypeHalfResult)
	h.SceneById = map[int32]masterdata.EntityMQuestScene{
		first:  {QuestId: testQuest, QuestSceneId: first, SortOrder: 1},
		result: {QuestId: testQuest, QuestSceneId: result, SortOrder: 2, QuestResultType: half},
		bridge: {QuestId: testQuest, QuestSceneId: bridge, SortOrder: 3},
		ending: {QuestId: testQuest, QuestSceneId: ending, SortOrder: 4},
		99:     {QuestId: testQuest + 1, QuestSceneId: 99, QuestResultType: half},
	}

	for _, tc := range []struct {
		name     string
		progress int32
		want     int32
	}{
		{"half result followed by field scenes", result, result},
		{"ordinary finish", ending, ending},
		{"missing progress", 0, ending},
		{"another quest's half result", 99, ending},
	} {
		t.Run(tc.name, func(t *testing.T) {
			user := testUser()
			user.MainQuest.CurrentQuestSceneId = first
			user.MainQuest.HeadQuestSceneId = first
			user.MainQuest.ProgressQuestSceneId = tc.progress
			h.HandleQuestFinish(user, testQuest, false, false, 1000)
			if got := user.Quests[testQuest].ClearCount; got != 1 {
				t.Fatalf("clear count = %d, want 1", got)
			}
			if m := user.MainQuest; m.CurrentQuestSceneId != tc.want || m.HeadQuestSceneId != tc.want {
				t.Fatalf("field current/head after victory = %d/%d, want %d", m.CurrentQuestSceneId, m.HeadQuestSceneId, tc.want)
			}
			if tc.want == result {
				for _, next := range []int32{bridge, ending} {
					h.HandleMainFlowSceneProgress(user, next, 2000)
					m := user.MainQuest
					if m.CurrentQuestSceneId != next || m.HeadQuestSceneId != next || m.IsReachedLastQuestScene != (next == ending) {
						t.Fatalf("after scene %d: current/head %d/%d, last scene reached %v", next, m.CurrentQuestSceneId, m.HeadQuestSceneId, m.IsReachedLastQuestScene)
					}
				}
			}
		})
	}
}
