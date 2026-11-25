#pragma once
#include <include/Ling.h>
#include "editor/shapes/ShapeBase.h"
#include "editor/NumberModel.h"
class ShapeNumber : public ShapeBase
{
public:
	ShapeNumber(WinPin* win);
	~ShapeNumber();
	void paint(ID2D1DeviceContext* ctx) override;
	void paintDragger(ID2D1DeviceContext* ctx) override;
	void mouseDrag(const float x, const float y) override;
	void mouseDown(const float x, const float y) override;
	void mouseUp(const float x, const float y) override;
	void mouseMove(const float x, const float y) override;
	void mouseWheel(const float x, const float y, const short delta) override;
	void setCursor() override;
	// 单击就是它的正常用法：落一个序号徽章，不需要拖动
	bool isValidWithoutDrag() override { return true; };
	std::uint64_t number() const noexcept { return model_.number(); }
    std::optional<std::uint64_t> ordinaryNumber() const noexcept override { return model_.number(); }
public:
private:
	void makePath();
	// 半径变化后要重排文字，两处调用点合到一起
	void makeTextLayout();
private:
	HuhuCapture::Editor::NumberModel model_;
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushText;
	Microsoft::WRL::ComPtr<IDWriteTextLayout> layoutText;
	bool isFill{ false }, isWheel{false};
};

