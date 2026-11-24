#include "pch.h"
#include "app/App.h"

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPTSTR lpCmdLine, _In_ int nCmdShow)
{
    App::init();
    MSG msg{};
    BOOL status{};
    while ((status = GetMessage(&msg, NULL, 0, 0)) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    App::dispose(); //趁 COM 还活着把单例拆掉，别留给 CoUninitialize 之后的静态析构
    Ling::dispose();
    return status == -1 ? 1 : static_cast<int>(msg.wParam);
}