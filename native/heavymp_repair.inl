// ETS2 1.62.3.3s native truck definition repair: constructor type 22c4908,
// virtual repair 8492e0. Execute on the local actor update thread only.
SRWLOCK repairLock = SRWLOCK_INIT;
std::array<unsigned char, 16> repairId{};
ULONGLONG repairReceived = 0, repairCompleted = 0;
bool repairPending = false, repairOkay = false;
unsigned repairFailure = 0;
DWORD repairException = 0;
const char* repairFailureReason() {
    switch (repairFailure) {
    case 1:
        return "no truck active in driving mode";
    case 2:
        return "truck configuration unavailable";
    case 3:
        return "unrecognized truck type";
    case 4:
        return "repair routine has changed";
    case 5:
        return "damage remained after repair";
    case 6:
        return "the game rejected the repair";
    case 7:
        return "timed out waiting for the game to resume";
    default:
        return "";
    }
}
bool repairLocalTruck(Ptr actor) {
    __try {
        repairFailure = 1;
        const Ptr physics = field<Ptr>(actor, 0x18);
        if (!readable(physics, 0x200) || field<Ptr>(physics, 0) != engine + 0x2332578)
            return false;
        repairFailure = 2;
        const Ptr damage = field<Ptr>(physics, 0x1f8);
        if (!readable(damage, 0x1b8))
            return false;
        repairFailure = 3;
        if (field<Ptr>(damage, 0) != engine + 0x2310420)
            return false;
        repairFailure = 4;
        if (field<std::uint64_t>(engine, 0x8392d0) != 0xfdb334528ec8348ULL)
            return false;
        // Call the fingerprint-validated original API. PAGE_EXECUTE_WRITECOPY is a
        // valid image protection; class/signature checks replace the old protection filter.
        call<void>(0x8392d0, damage);
        repairFailure = 5;
        for (unsigned at = 0x1a0; at <= 0x1b4; at += 4) {
            const float wear = field<float>(damage, at);
            if (!std::isfinite(wear) || std::abs(wear) > .0001f)
                return false;
        }
        repairFailure = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        repairException = GetExceptionCode();
        repairFailure = 6;
        return false;
    }
}
void processRepair(Ptr actor) {
    AcquireSRWLockExclusive(&repairLock);
    if (repairPending) {
        repairPending = false;
        repairException = 0;
        repairFailure = 7;
        repairOkay = GetTickCount64() - repairReceived < 10000 && repairLocalTruck(actor);
        repairCompleted = GetTickCount64();
        char line[240]{};
        sprintf_s(line, "[HeavyMP chat] Repair %s; stage=%u exception=%08lx reason=%s",
                  repairOkay ? "completed" : "unavailable", repairFailure, repairException,
                  repairFailureReason());
        log(line);
    }
    ReleaseSRWLockExclusive(&repairLock);
}
