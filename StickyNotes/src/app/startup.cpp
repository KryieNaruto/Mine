#include "app/startup.h"

#include <QFile>

#include "core/persistence.h"

// 返回值=「本次触发了播种」,非落盘成功:saveStore 可能失败(如只读/磁盘满),
// 但本会话内 Note 已入 store、窗口照常装配,下次启动文件仍缺失会再次播种。
bool seedFirstRunNoteIfMissing(NoteStore& store, const QString& storePath) {
  if (QFile::exists(storePath))
    return false;  // 文件已存在(含空数组/损坏)→ 尊重用户已有数据/删除,不干预
  store.add(Note{});  // 播种空白便签(默认橙黄 / pos(100,100) / 260×320),add 自动生成 id
  saveStore(store, storePath);
  return true;
}
