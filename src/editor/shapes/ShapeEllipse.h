#pragma once
#include <include/Ling.h>
#include "editor/shapes/ShapeBase.h"
#include "editor/BoxModel.h"
class ShapeEllipse : public ShapeBase
{
public:
	ShapeEllipse(WinPin* win);
	~ShapeEllipse();
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
	HuhuCapture::Editor::BoxModel model_{HuhuCapture::Editor::BoxKind::Ellipse};
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
	float strokeWidth{ 1.f };
	bool isFill{ false };
};

