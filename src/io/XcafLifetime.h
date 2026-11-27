#pragma once

namespace steplens::io {

// 在首次 GetApplication/NewDocument/Close 之后登记本 TU 的 companion 静态对象。
// companion 析构早于 OCCT TU 里的 locApp，只给出 locApp 销毁的上界，不是对 locApp
// 析构体本身的插桩，不能替代 ASan 或证明 Close 成功。
void noteXcafApplicationAccessed() noexcept;

// 可选主线程预热，只建立 mutex/App/companion，不启动 Retirer。
// 产品寿命保证在 Retirer::instance() 首次边界；main 不调用本函数也可以。
// 若再接 main，须放在 Interface_Static/Log 的 try 之后、窗口之前，不能放进可跳过的 try。
void primeXcafApplicationOnMainThread();

} // namespace steplens::io
