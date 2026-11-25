#pragma once
#include <include/Ling.h>
#include "editor/shapes/ShapeBase.h"
#include "editor/BoxModel.h"
#include "editor/StrokeModel.h"
class ShapeMosaic : public ShapeBase
{
public:
	ShapeMosaic(WinPin* win);
	~ShapeMosaic();
	void paint(ID2D1DeviceContext* ctx) override;
	void paintDragger(ID2D1DeviceContext* ctx) override;
	void mouseDrag(const float x, const float y) override;
	void mouseDown(const float x, const float y) override;
	void mouseUp(const float x, const float y) override;
	void mouseMove(const float x, const float y) override;
	void setCursor() override;
public:
private:
	void makePath();
	void resetMosaic();
	void buildMosaicBitmap();
	Microsoft::WRL::ComPtr<ID2D1Bitmap> createMosaicBitmap(int blockSize);
private:
	HuhuCapture::Editor::BoxModel box_;
	HuhuCapture::Editor::StrokeModel stroke_;
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
	// 圆头圆角描边，涂抹模式下让笔画首尾和拐点是圆的
	Microsoft::WRL::ComPtr<ID2D1StrokeStyle> roundStyle;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
	Microsoft::WRL::ComPtr<ID2D1Bitmap> mosaicBitmap;
	Microsoft::WRL::ComPtr<ID2D1BitmapBrush> mosaicBrush;
	// 马赛克位图只覆盖笔画包围盒那一小块，这是它左上角在窗口里的位置。
	// mosaicBrush 要按它平移，否则贴图会跑到窗口原点去。
	D2D1_POINT_2F mosaicOrigin{ 0.f, 0.f };
	float strokeWidth{ 1.f };
	bool isRect{ false };
};
