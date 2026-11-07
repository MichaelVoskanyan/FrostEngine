#pragma once

class EditorContext;

// The "Level" window (outliner): the level's name and its objects, as a tree: objects attached to
// another (see AttachmentComponent) are listed under it. Click selects, double-click frames the
// object in the viewport, right-click offers Focus/Duplicate/Detach/Delete. Models dropped onto
// the list are added in front of the camera; a model dropped onto an object is added attached to
// it. Dragging an object onto another attaches it there (at its mount), and out onto the list's
// empty space detaches it.
class LevelPanel final {
public:
    static constexpr const char* kWindowName = "Level";

    void Draw(EditorContext& context);
};
