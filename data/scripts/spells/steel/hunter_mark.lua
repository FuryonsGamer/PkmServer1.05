local AREA_COUNTER = {
    {0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0},
    {0, 0, 1, 1, 1, 0, 0},
    {0, 0, 1, 2, 1, 0, 0},
    {0, 0, 1, 1, 1, 0, 0},
    {0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0}
}

local AREA_COUNTER2 = {
    {0, 0, 0, 0, 0, 0, 0},
    {0, 0, 1, 1, 1, 0, 0},
    {0, 1, 0, 0, 0, 1, 0},
    {0, 1, 0, 2, 0, 1, 0},
    {0, 1, 0, 0, 0, 1, 0},
    {0, 0, 1, 1, 1, 0, 0},
    {0, 0, 0, 0, 0, 0, 0}
}

local combat1 = createCombatObject()
combat1:setParameter(COMBAT_PARAM_TYPE, COMBAT_STEELDAMAGE)
combat1:setArea(createCombatArea(AREA_COUNTER))
combat1:setParameter(COMBAT_PARAM_EFFECT, 2286)
combat1:setStringParameter(COMBAT_PARAM_STRING_SPELLNAME, "Hunter Mark")

local combat2 = createCombatObject()
combat2:setParameter(COMBAT_PARAM_TYPE, COMBAT_STEELDAMAGE)
combat2:setArea(createCombatArea(AREA_COUNTER2))
combat2:setParameter(COMBAT_PARAM_EFFECT, 2286)
combat2:setStringParameter(COMBAT_PARAM_STRING_SPELLNAME, "Hunter Mark")

local spell = Spell(SPELL_INSTANT)

function spell.onCastSpell(creature, variant)
    local creature_id = creature:getId()
    
    creature:sendJump(20, 500)
    
    addEvent(function()
        if Creature(creature_id) then
            combat1:execute(Creature(creature_id), variant)
        end
    end, 600)
    
    addEvent(function()
        if Creature(creature_id) then
            combat2:execute(Creature(creature_id), variant)
        end
    end, 700)

    return true
end

spell:name("Hunter Mark")
spell:words("### Hunter Mark ###")
spell:isAggressive(true)
spell:needLearn(false)
spell:register()
