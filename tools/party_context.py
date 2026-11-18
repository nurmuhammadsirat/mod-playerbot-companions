"""Optional read-only relationship context for party dialogue."""

import re
import time
from html import escape

_RECALL_COUNT = 3
_DAY_SECONDS = 86400
_LOYALTY_GRACE_DAYS = 7
_AGE_LABELS = (
    (3600, 'less than an hour ago'),
    (_DAY_SECONDS, 'earlier today'),
    (2 * _DAY_SECONDS, 'yesterday'),
    (7 * _DAY_SECONDS, 'a few days ago'),
    (14 * _DAY_SECONDS, 'about a week ago'),
    (30 * _DAY_SECONDS, 'a few weeks ago'),
    (60 * _DAY_SECONDS, 'about a month ago'),
    (365 * _DAY_SECONDS, 'a few months ago'),
)


def _memory_age_label(elapsed):
    return next((label for limit, label in _AGE_LABELS if elapsed < limit),
                'over a year ago')


def _read_context(characters_db, open_context_db, owner_guid, bot_guid, map_id,
                  now):
    with characters_db.cursor() as cursor:
        cursor.execute(
            "SELECT guid, UNIX_TIMESTAMP(creation_date) "
            "FROM characters WHERE guid IN (%s, %s) AND deleteDate IS NULL",
            (owner_guid, bot_guid),
        )
        created_at = dict(cursor.fetchall())
    if not created_at.get(owner_guid) or not created_at.get(bot_guid):
        return []

    with open_context_db() as journal, journal.cursor(dictionary=True) as cursor:
        cursor.execute(
            "SELECT c.loyalty, c.last_shared_at, "
            "(SELECT COUNT(*) FROM playerbot_companion_memories a "
            "WHERE a.owner_guid = c.owner_guid AND a.owner_created_at = c.owner_created_at "
            "AND a.bot_guid = c.bot_guid AND a.bot_created_at = c.bot_created_at "
            "AND a.memory_type = 'adventure' AND a.occurred_at BETWEEN %s AND %s) AS recent_adventures, "
            "m.memory_type, m.map_id, "
            "m.instance_id, m.occurred_at, m.summary "
            "FROM playerbot_companions c "
            "LEFT JOIN playerbot_companion_memories m "
            "ON m.owner_guid = c.owner_guid AND m.owner_created_at = c.owner_created_at "
            "AND m.bot_guid = c.bot_guid AND m.bot_created_at = c.bot_created_at "
            "WHERE c.owner_guid = %s AND c.owner_created_at = %s "
            "AND c.bot_guid = %s AND c.bot_created_at = %s "
            "ORDER BY (m.map_id = %s) DESC, m.occurred_at DESC, m.id DESC LIMIT 8",
            (now - _LOYALTY_GRACE_DAYS * _DAY_SECONDS, now,
             owner_guid, created_at[owner_guid], bot_guid, created_at[bot_guid], map_id),
        )
        return cursor.fetchall()


def _relationship_tone(loyalty):
    if loyalty >= 80:
        return 'Speak with the ease of a longstanding, devoted companion.'
    if loyalty >= 50:
        return 'Speak warmly with the familiarity of a trusted companion.'
    if loyalty >= 20:
        return 'Speak with the familiarity of someone who has traveled with them before.'
    return 'Speak with a little reserve, in keeping with what you know of this companion.'


def _relationship_notes(row, now):
    stored_loyalty = int(row['loyalty'])
    recent_adventures = int(row['recent_adventures'])
    elapsed = max(0, now - int(row['last_shared_at']))
    inactive_days = elapsed // _DAY_SECONDS
    loyalty = max(0, stored_loyalty - max(0, inactive_days - _LOYALTY_GRACE_DAYS))
    notes = [_relationship_tone(loyalty)]
    if loyalty < stored_loyalty:
        notes.append(
            f"Their last shared adventure was {_memory_age_label(elapsed)}. "
            "Some familiarity has faded with time apart. Keep the warmth that remains and sound a little "
            "less at ease as they find their rhythm together again. Acknowledge the gap gently if it fits."
        )
    elif recent_adventures >= 2:
        notes.append(
            "They have adventured together repeatedly in the past week. "
            "Let the practice of working together come through in the ease and confidence of your words."
        )
    elif recent_adventures:
        notes.append(
            "They recently shared an adventure. Let that time together show through a little familiarity "
            "when the conversation calls for it."
        )
    return notes


def _memory_lines(rows, now):
    # A final boss and the completion from that kill share one prompt entry.
    completions = {
        (row['map_id'], row['instance_id'], row['occurred_at'])
        for row in rows if row['memory_type'] == 'dungeon_completion'
    }
    memories = {}
    seen = set()
    for row in rows:
        summary = row['summary']
        if not isinstance(summary, str):
            continue
        summary = ' '.join(re.sub(r'[\x00-\x1f\x7f-\x9f]', '', summary).split())
        if not summary or summary.casefold() in seen:
            continue
        seen.add(summary.casefold())
        moment = row['occurred_at']
        encounter = (row['map_id'], row['instance_id'], moment)
        key = encounter if (
            encounter in completions and row['memory_type'] in ('boss_kill', 'dungeon_completion')
        ) else (moment, summary)
        memories.setdefault(key, (moment, []))[1].append(escape(summary, quote=False))

    lines = []
    for moment, summaries in list(memories.values())[:_RECALL_COUNT]:
        age = _memory_age_label(now - int(moment))
        lines.append(f"- ({age}) {' '.join(summaries)}")
    return lines


def get_party_context(*, characters_db, open_context_db, player_guid,
                      speaker_guid, player_name, speaker_name, map_id):
    """Read the current relationship and render its guidance for the speaker."""
    now = int(time.time())
    rows = _read_context(
        characters_db, open_context_db, player_guid, speaker_guid, map_id, now,
    )
    if not rows:
        return None

    bot = escape(speaker_name, quote=False)
    player = escape(player_name, quote=False)
    lines = [
        '<companion_context>',
        f'{bot} is a companion of {player}.',
        *_relationship_notes(rows[0], now),
        f'Apply this familiarity only to {bot} addressing {player}. '
        'Keep the existing personality and tone.',
        'Show the bond mostly through word choice. Mention it only when it fits '
        'the conversation, and avoid repeating a remark from recent chat. '
        'Keep loyalty scores and game rules out of dialogue.',
        'Ground any claimed shared experiences in the available memories.',
    ]
    memories = _memory_lines(rows, now)
    if memories:
        lines.extend([
            f'Only {bot} can recall these shared experiences with {player} '
            'as their own.',
            'When the player asks about a matching event, answer from the '
            'relevant memory.',
            'Otherwise mention a memory only if it fits naturally. '
            'Paraphrase and keep it brief.',
            *memories,
        ])
    lines.append('</companion_context>')
    return {
        'text': '\n'.join(lines),
        'reunion_tone': (
            'Let your current bond guide how familiar the greeting feels.'
        ),
    }
