#ifndef WELYOS_CRED_H
#define WELYOS_CRED_H
#include <stdint.h>

/*
 * 身分與特權(credentials)。
 *
 * 每個行程帶著「它是誰」(uid、gid)與「它被允許做哪些特別的事」(caps)。
 * 檔案系統等需要判斷權限的地方,只問 cred_current():現在這條執行緒代表誰。
 * 它們因此不必認得「行程」這個結構(proc.c 提供實作,其他模組只引用本檔)。
 *
 * uid 0 傳統上稱為 root。但這裡真正決定「能不能做」的不是 uid,而是 caps 裡的位元:
 * 一個特權行程可以只留下它需要的那幾項,其餘永久放棄(最小權限原則)—
 * 之後即使它被攻破,能造成的損害也只限於留下的那幾項。
 * 特權只能減少、不能增加;uid 改成非 0 時全部清除。
 */
struct cred {
    uint32_t uid;                /* 使用者編號;0 = root */
    uint32_t gid;                /* 群組編號 */
    uint32_t caps;               /* 特權(CAP_* 的組合) */
};

#define CAP_FS     1u            /* 不受檔案權限限制;可以更改任何檔案的擁有者與權限 */
#define CAP_KILL   2u            /* 可以對別人的行程送訊號 */
#define CAP_SETID  4u            /* 可以把自己的 uid / gid 改成任何值 */
#define CAP_ADMIN  8u            /* 系統管理(保留給之後的功能,例如設定網路) */
#define CAP_ALL    15u

/* 目前這條執行緒的身分。NULL = kernel 自己(不受任何限制) */
const struct cred *cred_current(void);
#endif
