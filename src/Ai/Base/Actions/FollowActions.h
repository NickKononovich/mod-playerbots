/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_FOLLOWACTIONS_H
#define PLAYERBOTS_FOLLOWACTIONS_H

#include "MovementActions.h"

class PlayerbotAI;

class FollowAction : public MovementAction
{
public:
    FollowAction(PlayerbotAI* botAI, std::string const name = "follow") : MovementAction(botAI, name) {}

    bool Execute(Event event) override;
    bool isUseful() override;
    bool CanDeadFollow(Unit* target);
    bool MemberHandIn(bool act);  // [quest17] [quest18]
    bool TakeTried(uint32 questId, bool mark);  // [group23]

private:
    // [follow57] a guild follower judged solo (no member within the group reward distance: SoloSafety::Applies)
    // walks to its leader by its priced walk, a leg in sight at a time, or stands when that walk passes a reach it
    // must not fight; true: the tick is used
    bool FarFollowWalk(std::string const& target);
    bool _farLeg = false;        // [follow57] the leg under way is its priced walk's
    ObjectGuid _farRefusedLog;   // [follow57] one refusal line per leader
};

#endif
