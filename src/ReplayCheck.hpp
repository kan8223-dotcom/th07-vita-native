#pragma once
// Replay playback hooks (ReplayManager / GameManager). Empty in this build: they
// read nothing and change nothing, so replay playback is the engine's own.
struct ReplayManager;

namespace ReplayCheck
{
void StageBegin(const ReplayManager *mgr, bool firstStage);
void StageMissing(const ReplayManager *mgr);
void Tick(const ReplayManager *mgr);
void StageEnd();
void End(const ReplayManager *mgr);
} // namespace ReplayCheck
