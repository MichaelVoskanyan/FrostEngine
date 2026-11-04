#pragma once

class EditorContext;

// The "Level" window (outliner): the level's name and its objects. Click selects, double-click
// frames the object in the viewport, right-click offers Focus/Duplicate/Delete. Models dropped
// onto the list are added in front of the camera.
class LevelPanel final {
public:
    static constexpr const char* kWindowName = "Level";

    void Draw(EditorContext& context);
};
