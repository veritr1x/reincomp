#pragma once

namespace bunker::windows {
enum class GamePageAction { Init, Frame, Position, Size, Load, Show, Hide, Destroy, Back, Forward, Reload, Stop, Alpha, Background, Header, ClearCache, ExternalLinks };
using PostGamePage=void(*)(GamePageAction,const wchar_t*,const wchar_t*,int,int,int,int,const wchar_t*);
using QueryGamePage=int(*)(const wchar_t*,int,wchar_t*,int);
}
