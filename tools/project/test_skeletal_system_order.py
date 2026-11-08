"""Guard the shared current-frame gameplay-to-skeletal-animation contract."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SkeletalSystemOrder(unittest.TestCase):
    def test_native_gameplay_writes_reach_animation_in_the_same_frame(self):
        source = (ROOT / 'src/app/Application.cpp').read_text(encoding='utf-8')
        script = source.index('RegisterSystem(std::move(scriptSystem))')
        animation = source.index('RegisterSystem(std::make_unique<aether::AnimationSystem>())')
        self.assertLess(script, animation)
        self.assertLess(source.index('RegisterSystem(std::move(spriteAnimationSystem))'), script)
        self.assertLess(source.index('RegisterSystem(std::move(physicsSystem))'), script)
        self.assertLess(animation, source.index('RegisterSystem(std::move(cameraSystem))'))

    def test_web_uses_the_same_order_and_reverse_teardown(self):
        source = (ROOT / 'src/app/web/WebScene.cpp').read_text(encoding='utf-8')
        script = source.index('world.RegisterSystem(std::move(script))')
        animation = source.index('world.RegisterSystem(std::move(animation))')
        self.assertLess(script, animation)
        self.assertLess(source.index('world.RegisterSystem(std::move(physics))'), script)
        self.assertLess(source.index('world.RegisterSystem(std::make_unique<SpriteAnimationSystem>'), script)
        self.assertLess(animation, source.index('world.RegisterSystem(std::move(particles))'))
        self.assertLess(source.index('world.UnregisterSystem("AnimationSystem")'), source.index('world.UnregisterSystem("WebScriptSystem")'))


if __name__ == '__main__':
    unittest.main()
