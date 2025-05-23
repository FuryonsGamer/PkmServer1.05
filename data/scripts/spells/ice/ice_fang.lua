local combat = createCombatObject()
combat:setParameter(COMBAT_PARAM_TYPE, COMBAT_ICEDAMAGE)
combat:setParameter(COMBAT_PARAM_DISTANCEEFFECT, 29)
combat:setParameter(COMBAT_PARAM_EFFECT, 774)
combat:setStringParameter(COMBAT_PARAM_STRING_SPELLNAME, "Ice Fang")

local spell = Spell(SPELL_INSTANT)

function spell.onCastSpell(creature, variant)

    combat:execute(creature, variant)

    return true
end

spell:name("Ice Fang")
spell:words("#Ice Fang#")
spell:isAggressive(true)
spell:needLearn(false)
spell:range(3)
spell:needTarget(true)
spell:register()
