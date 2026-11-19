"""Regression checks for companion relationship prompts."""

import sys
import unittest
from pathlib import Path
from unittest.mock import MagicMock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import party_context as companions


NOW = 1_800_000_000


class CompanionContextTests(unittest.TestCase):
    def setUp(self):
        self.db = MagicMock()
        self.identity_cursor = self.db.cursor.return_value.__enter__.return_value
        self.identity_cursor.fetchall.return_value = [(42, 1_600_000_001), (77, 1_600_000_002)]
        self.companion_db = MagicMock()
        self.connect = MagicMock(return_value=self.companion_db)
        self.companion_db.__enter__.return_value = self.companion_db
        self.journal_cursor = self.companion_db.cursor.return_value.__enter__.return_value
        self.journal_cursor.fetchall.return_value = []

    def context(self):
        result = companions.get_party_context(
            characters_db=self.db, open_context_db=self.connect,
            player_guid=42, speaker_guid=77, player_name='Kara',
            speaker_name='Darrin', map_id=36,
        )
        return result['text'] if result else ''

    def test_missing_character_never_opens_companion_database(self):
        self.identity_cursor.fetchall.return_value = [(42, 1_600_000_001)]
        self.assertEqual(self.context(), '')
        self.connect.assert_not_called()

    def test_missing_relationship_returns_no_context(self):
        self.assertEqual(self.context(), '')
        self.companion_db.__exit__.assert_called_once()

    def test_matching_pair_uses_decayed_loyalty_and_combines_final_encounter(self):
        event = {
            'loyalty': 50,
            'last_shared_at': NOW - 8 * 86400,
            'recent_adventures': 0,
            'map_id': 36,
            'instance_id': 92,
            'occurred_at': NOW - 86400,
        }
        self.journal_cursor.fetchall.return_value = [
            {**event, 'memory_type': 'dungeon_completion', 'summary': 'Completed the Deadmines together.'},
            {**event, 'memory_type': 'boss_kill', 'summary': 'Defeated VanCleef together.'},
            {**event, 'memory_type': 'boss_kill', 'summary': 'Defeated VanCleef together.'},
        ]
        with patch.object(companions.time, 'time', return_value=NOW):
            context = self.context()

        self.assertEqual(self.identity_cursor.execute.call_args.args[1], (42, 77))
        self.assertEqual(
            self.journal_cursor.execute.call_args.args[1],
            (NOW - 7 * 86400, NOW, 42, 1_600_000_001, 77, 1_600_000_002, 36),
        )
        self.assertIn('familiarity of someone who has traveled', context)
        self.assertNotIn('trusted companion', context)
        self.assertIn('familiarity has faded with time apart', context)
        self.assertIn('Only Darrin can recall', context)
        self.assertIn('Completed the Deadmines together. Defeated VanCleef together.', context)
        self.assertEqual(context.count('VanCleef'), 1)

    def test_relationship_warms_cools_and_warms_again(self):
        row = {
            'loyalty': 45,
            'last_shared_at': NOW,
            'recent_adventures': 2,
            'memory_type': None,
            'summary': None,
        }
        self.journal_cursor.fetchall.return_value = [row]
        with patch.object(companions.time, 'time', return_value=NOW) as clock:
            recent = self.context()
            self.assertIn('adventured together repeatedly', recent)
            self.assertIn('familiarity of someone who has traveled', recent)

            row['loyalty'] = 50
            self.assertIn('trusted companion', self.context())

            clock.return_value = NOW + 8 * 86400 - 1
            row['recent_adventures'] = 0
            self.assertIn('trusted companion', self.context())
            self.assertNotIn('familiarity has faded', self.context())

            clock.return_value += 1
            cooled = self.context()
            self.assertIn('familiarity has faded with time apart', cooled)
            self.assertNotIn('trusted companion', cooled)

            row.update(loyalty=54, last_shared_at=clock.return_value, recent_adventures=1)
            returned = self.context()
            self.assertIn('trusted companion', returned)
            self.assertIn('recently shared an adventure', returned)
            self.assertNotIn('familiarity has faded', returned)

    def test_unplayed_relationship_has_no_activity_cue(self):
        self.journal_cursor.fetchall.return_value = [{
            'loyalty': 0, 'last_shared_at': 0, 'recent_adventures': 0,
            'memory_type': None, 'summary': None,
        }]
        context = self.context()
        self.assertIn('Speak with a little reserve', context)
        self.assertNotIn('familiarity has faded', context)
        self.assertNotIn('shared an adventure', context)

    def test_inactivity_is_reflected_without_crossing_a_tier(self):
        self.journal_cursor.fetchall.return_value = [{
            'loyalty': 90, 'last_shared_at': NOW - 8 * 86400, 'recent_adventures': 0,
            'memory_type': None, 'summary': None,
        }]
        with patch.object(companions.time, 'time', return_value=NOW):
            context = self.context()
        self.assertIn('devoted companion', context)
        self.assertIn('Keep the warmth that remains', context)
        self.assertIn('familiarity has faded with time apart', context)


class ChatTraditionTests(unittest.TestCase):
    def setUp(self):
        pending = patch.object(companions, '_pending', {})
        pending.start()
        self.addCleanup(pending.stop)
        self.key = (42, 100, 77, 200)
        self.journal = MagicMock()
        self.journal.__enter__.return_value = self.journal
        self.cursor = self.journal.cursor.return_value.__enter__.return_value
        self.connect = MagicMock(return_value=self.journal)
        self.ask_model = MagicMock(return_value=(
            '{"kind":"rendezvous","text":"the Goldshire inn"}'
        ))
        self.was_delivered = MagicMock(return_value=True)

    def reply(self, message, event_id=1, group_id=9, bot_count=1, key=None):
        return companions._handle_party_reply(
            self.connect, key or self.key, 'Darrin', event_id=event_id,
            group_id=group_id, message=message, addressed=True,
            bot_count=bot_count, ask_model=self.ask_model,
            was_delivered=self.was_delivered,
        )

    def test_save_requires_delivered_confirmation_for_the_same_pair_and_group(self):
        self.cursor.fetchone.side_effect = [(1,), None, (1,), None]
        prompt = self.reply('Make the Goldshire inn our usual spot')
        self.assertIn('the Goldshire inn', prompt)
        self.journal.commit.assert_not_called()
        self.assertIsNone(self.reply('yes', 2, group_id=10))
        self.assertIsNone(self.reply('yes', 2, key=(43, 100, 77, 200)))
        self.was_delivered.return_value = False
        self.assertIsNone(self.reply('yes', 2))
        self.journal.commit.assert_not_called()
        self.was_delivered.return_value = True
        self.assertIn('put it in our journal', self.reply('Yes!', 3))
        self.was_delivered.assert_called_with(1)
        query, args = self.cursor.execute.call_args.args
        self.assertIn('INSERT INTO playerbot_companion_memories', query)
        self.assertEqual(args, (*self.key, 3,
                                'Our favorite place to regroup is "the Goldshire inn".'))
        self.journal.commit.assert_called_once()
        self.assertIsNone(self.reply('yes', 4))

    def test_multiple_bots_require_named_proposal_and_confirmation(self):
        self.cursor.fetchone.side_effect = [(1,), None, (1,), None]
        self.assertIsNone(self.reply('Make the Goldshire inn our spot', bot_count=2))
        self.ask_model.assert_not_called()
        self.assertIn('yes, Darrin', self.reply(
            'Darrin, make the Goldshire inn our spot', 2, bot_count=2,
        ))
        self.assertIsNone(self.reply('yes', 3, bot_count=2))
        self.assertIn('put it in our journal', self.reply('Yes, Darrin!', 4, bot_count=2))

    def test_changed_tradition_is_not_overwritten(self):
        self.cursor.fetchone.side_effect = [(1,), (11, 'Old'), (1,), (12, 'New')]
        self.assertIn('replace', self.reply('Make the Goldshire inn our spot'))
        self.assertIn('journal has changed', self.reply('yes', 2))
        self.assertEqual(self.cursor.execute.call_count, 4)
        self.journal.rollback.assert_called_once()
        self.journal.commit.assert_not_called()

    def test_explicit_replacement_deletes_old_entry_in_same_transaction(self):
        old = (11, 'Old')
        self.cursor.fetchone.side_effect = [(1,), old, (1,), old]
        self.assertIn('replace', self.reply('Make the Goldshire inn our spot'))
        self.assertIn('put it in our journal', self.reply('yes', 2))
        deletion, insertion = self.cursor.execute.call_args_list[-2:]
        self.assertIn('DELETE FROM playerbot_companion_memories', deletion.args[0])
        self.assertEqual(deletion.args[1], (11,))
        self.assertIn('INSERT INTO playerbot_companion_memories', insertion.args[0])
        self.journal.start_transaction.assert_called_once()
        self.journal.commit.assert_called_once()

    def test_proposal_expires_and_model_cannot_invent_the_value(self):
        self.assertIsNone(self.reply('Let’s try the Lion’s Pride'))
        self.connect.assert_not_called()
        self.cursor.fetchone.side_effect = [(1,), None]
        with patch.object(companions.time, 'monotonic', return_value=10) as clock:
            self.assertTrue(self.reply('Make the Goldshire inn our usual spot', 2))
            clock.return_value += companions._PROPOSAL_SECONDS
            self.assertIsNone(self.reply('yes', 3))
        self.journal.commit.assert_not_called()


if __name__ == '__main__':
    unittest.main()
