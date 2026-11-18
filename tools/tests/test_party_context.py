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


if __name__ == '__main__':
    unittest.main()
