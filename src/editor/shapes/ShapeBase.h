#pragma once
#include <include/Ling.h>
#include "editor/Geometry.h"
#include <span>
class WinPin;
class ShapeBase
{
public:
	ShapeBase(WinPin* win);
	virtual ~ShapeBase();
	virtual void paint(ID2D1DeviceContext* ctx) = 0;
	virtual void paintDragger(ID2D1DeviceContext* ctx) {};
	virtual void mouseMove(const float x, const float y) { };
	virtual void mouseDrag(const float x, const float y) {};
	virtual void mouseDown(const float x, const float y) { };
	virtual void mouseUp(const float x, const float y) { };
	virtual void mouseWheel(const float x, const float y, const short delta) {};
	virtual void setCursor() {};
	// 新建这一笔如果只是按下马上弹起（没有拖动），默认当成什么也没画，元素直接丢掉。
	// 单击本身就是正常用法的元素（number 落徽章、text 进编辑）覆盖它返回 true
	virtual bool isValidWithoutDrag() { return false; };
	bool isInRect(const D2D1_RECT_F rect, const float x, const float y);
public:
	WinPin* win;
	int hoverDraggerIndex{ -1 };
protected:
    // Convert standard geometry in bounded batches: no aliasing cast, no
    // duplicated persistent point buffer and no per-vertex COM call overhead.
    static void addPathLines(ID2D1GeometrySink* sink,
        std::span<const HuhuCapture::Editor::EditorPoint> points);
protected:
	float draggerSize;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushDragger;
private:
};

