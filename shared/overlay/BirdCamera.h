#pragma once

namespace advancedfx {
namespace overlay {

// Bird camera transition system
// This updates the generic CameraOverride state with bird-eye transitions

// Start a bird transition from controller A to controller B
void BirdCamera_StartGoto(int fromControllerIdx, int toControllerIdx, float height = 1000.0f);

// Start bird-eye view of current player (with smooth transition)
void BirdCamera_StartPlayer(int controllerIdx, float height = 1000.0f);

// Return from bird-eye view to player POV
void BirdCamera_StartPlayerReturn();

// Stop bird camera and return control
void BirdCamera_Stop();

// Set target data for current frame (call before Update)
// For bird view: pass player eye position + height, with angles looking down
// For normal view: pass player eye position and actual eye angles
void BirdCamera_SetTargetA(const float pos[3], const float ang[3], bool valid);
void BirdCamera_SetTargetB(const float pos[3], const float ang[3], bool valid);

// Update bird camera state (call each frame from overlay after setting targets)
// Returns true if camera override should be active
bool BirdCamera_Update(float deltaTime, float curTime);

// Check if bird camera is currently active
bool BirdCamera_IsActive();

// Get the controller indices being tracked (returns false if not active)
bool BirdCamera_GetControllerIndices(int& outFromIdx, int& outToIdx);

// Check if a spec command needs to be executed and get the target controller index
// Returns -1 if no spec command is needed, otherwise returns the controller index to spec
int BirdCamera_GetPendingSpecTarget();

} // namespace overlay
} // namespace advancedfx
