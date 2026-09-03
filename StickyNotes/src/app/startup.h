#pragma once
#include <QString>
#include "core/note_store.h"
// 首次启动策略:store 文件不存在(真首启)时播种一张空白便签并落盘,保证开箱即有界面。
// 文件已存在(含空 {"notes":[]} 或损坏)一律不干预,尊重用户已有数据/删除,避免覆盖。
// 返回 true 表示「本次启动了播种」;落盘可能失败,失败时本会话窗口仍在、下次仍会播种。
bool seedFirstRunNoteIfMissing(NoteStore& store, const QString& storePath);
