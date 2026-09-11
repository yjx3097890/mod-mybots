-- Seed quest scripts and a sample patrol for P2/P2.5 demos.
-- Human starter zone examples (quest IDs are classic WotLK; adjust if your DB differs).

INSERT INTO `mybots_quest_script` (`quest_id`, `script`, `updated_at`) VALUES
(7, '{"steps":[{"op":"move_to","detail":"{\\"entry\\":197}"},{"op":"accept_quest","detail":"{\\"questId\\":7,\\"entry\\":197}"},{"op":"until","detail":"{\\"questId\\":7}"},{"op":"move_to","detail":"{\\"entry\\":197}"},{"op":"turnin_quest","detail":"{\\"questId\\":7,\\"entry\\":197}"}]}', UNIX_TIMESTAMP()),
(33, '{"steps":[{"op":"move_to","detail":"{\\"entry\\":198}"},{"op":"accept_quest","detail":"{\\"questId\\":33,\\"entry\\":198}"},{"op":"until","detail":"{\\"questId\\":33}"},{"op":"move_to","detail":"{\\"entry\\":198}"},{"op":"turnin_quest","detail":"{\\"questId\\":33,\\"entry\\":198}"}]}', UNIX_TIMESTAMP()),
(21, '{"name":"skirmish_at_echo_ridge","steps":[{"op":"move_to","detail":"{\\"entry\\":823}"},{"op":"accept_quest","detail":"{\\"questId\\":21,\\"entry\\":823}"},{"op":"until","detail":"{\\"questId\\":21}"},{"op":"move_to","detail":"{\\"entry\\":823}"},{"op":"turnin_quest","detail":"{\\"questId\\":21,\\"entry\\":823}"}]}', UNIX_TIMESTAMP())
ON DUPLICATE KEY UPDATE `script`=VALUES(`script`), `updated_at`=VALUES(`updated_at`);

INSERT INTO `mybots_patrol` (`id`, `name`, `waypoints`, `account_id`) VALUES
('demo-northshire', 'Northshire demo loop', '[{"x":-8895.0,"y":-133.0,"z":80.0,"wait":2},{"x":-8949.0,"y":-132.0,"z":83.0,"wait":2},{"x":-8910.0,"y":-160.0,"z":81.0,"wait":2}]', 0)
ON DUPLICATE KEY UPDATE `name`=VALUES(`name`), `waypoints`=VALUES(`waypoints`);
