#pragma once
#include <include/Ling.h>
#include "editor/shapes/ShapeBase.h"
#include "editor/ArrowModel.h"
class ShapeArrow : public ShapeBase
{
public:
	ShapeArrow(WinPin* win);
	~ShapeArrow();
	void paint(ID2D1DeviceContext* ctx) override;
	void paintDragger(ID2D1DeviceContext* ctx) override;
	void mouseDrag(const float x, const float y) override;
	void mouseDown(const float x, const float y) override;
	void mouseUp(const float x, const float y) override;
	void mouseMove(const float x, const float y) override;
	void mouseWheel(const float x, const float y, const short delta) override;
	void setCursor() override;
public:
private:
	void makeArrow();
private:
	HuhuCapture::Editor::ArrowModel model_;
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
	bool isFill{ false };
};

