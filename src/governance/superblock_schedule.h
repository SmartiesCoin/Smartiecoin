// Copyright (c) 2026 The Smartiecoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_GOVERNANCE_SUPERBLOCK_SCHEDULE_H
#define BITCOIN_GOVERNANCE_SUPERBLOCK_SCHEDULE_H

namespace superblock_schedule {
/** Whether the height is eligible for a superblock under the selected chain parameters. */
bool IsValidBlockHeight(int nBlockHeight);
/** Payment cycle at the height under the selected chain parameters. */
int GetPaymentCycle(int nBlockHeight);
} // namespace superblock_schedule

#endif // BITCOIN_GOVERNANCE_SUPERBLOCK_SCHEDULE_H
